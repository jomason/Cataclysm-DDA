#include "mongroup.h"
#include <vector>

#include "rng.h"
#include "debug.h"
#include "options.h"
#include "monstergenerator.h"
#include "json.h"
#include "mtype.h"
#include "calendar.h"
#include "assign.h"

//  Frequency: If you don't use the whole 1000 points of frequency for each of
//     the monsters, the remaining points will go to the defaultMonster.
//     Ie. a group with 1 monster at frequency will have 50% chance to spawn
//     the default monster.
//     In the same spirit, if you have a total point count of over 1000, the
//     default monster will never get picked, and nor will the others past the
//     monster that makes the point count go over 1000

std::map<mongroup_id, MonsterGroup> MonsterGroupManager::monsterGroupMap;
MonsterGroupManager::t_string_set MonsterGroupManager::monster_blacklist;
MonsterGroupManager::t_string_set MonsterGroupManager::monster_whitelist;
MonsterGroupManager::t_string_set MonsterGroupManager::monster_categories_blacklist;
MonsterGroupManager::t_string_set MonsterGroupManager::monster_categories_whitelist;
bool monster_whitelist_is_exclusive = false;

/** @relates string_id */
template<>
bool string_id<MonsterGroup>::is_valid() const
{
    return MonsterGroupManager::isValidMonsterGroup( *this );
}

/** @relates string_id */
template<>
const MonsterGroup &string_id<MonsterGroup>::obj() const
{
    return MonsterGroupManager::GetMonsterGroup( *this );
}

bool mongroup::is_safe() const
{
    return type.obj().is_safe;
}

bool mongroup::empty() const
{
    return population <= 0 && monsters.empty();
}

void mongroup::clear()
{
    population = 0;
    monsters.clear();
}

const MonsterGroup &MonsterGroupManager::GetUpgradedMonsterGroup( const mongroup_id &group )
{
    const MonsterGroup *groupptr = &group.obj();
    if( get_option<float>( "MONSTER_UPGRADE_FACTOR" ) > 0 ) {
        const time_duration replace_time = groupptr->monster_group_time *
                                           get_option<float>( "MONSTER_UPGRADE_FACTOR" );
        while( groupptr->replace_monster_group &&
               calendar::turn - calendar::time_of_cataclysm > replace_time ) {
            groupptr = &groupptr->new_monster_group.obj();
        }
    }
    return *groupptr;
}

//Quantity is adjusted directly as a side effect of this function
MonsterGroupResult MonsterGroupManager::GetResultFromGroup(
    const mongroup_id &group_name, int *quantity )
{
    int spawn_chance = rng( 1, 1000 );
    auto &group = GetUpgradedMonsterGroup( group_name );
    //Our spawn details specify, by default, a single instance of the default monster
    MonsterGroupResult spawn_details = MonsterGroupResult( group.defaultMonster, 1 );

    bool monster_found = false;
    // Step through spawn definitions from the monster group until one is found or
    for( auto it = group.monsters.begin(); it != group.monsters.end() && !monster_found; ++it ) {
        const mtype &mt = it->name.obj();
        // There's a lot of conditions to work through to see if this spawn definition is valid
        bool valid_entry = true;
        // If we are in classic mode, require the monster type to be either CLASSIC or WILDLIFE
        if( get_option<bool>( "CLASSIC_ZOMBIES" ) ) {
            valid_entry = valid_entry && ( mt.in_category( "CLASSIC" ) ||
                                           mt.in_category( "WILDLIFE" ) );
        }
        //Insure that the time is not before the spawn first appears or after it stops appearing
        valid_entry = valid_entry && ( calendar::time_of_cataclysm + it->starts < calendar::turn );
        valid_entry = valid_entry && ( it->lasts_forever() ||
                                       calendar::time_of_cataclysm + it->ends > calendar::turn );

        std::vector<std::pair<time_point, time_point> > valid_times_of_day;
        bool season_limited = false;
        bool season_matched = false;
        //Collect the various spawn conditions, and then insure they are met appropriately
        for( auto &elem : it->conditions ) {
            //Collect valid time of day ranges
            if( ( elem ) == "DAY" || ( elem ) == "NIGHT" || ( elem ) == "DUSK" ||
                ( elem ) == "DAWN" ) {
                const time_point sunset = calendar::turn.sunset();
                const time_point sunrise = calendar::turn.sunrise();
                if( ( elem ) == "DAY" ) {
                    valid_times_of_day.push_back( std::make_pair( sunrise, sunset ) );
                } else if( ( elem ) == "NIGHT" ) {
                    valid_times_of_day.push_back( std::make_pair( sunset, sunrise ) );
                } else if( ( elem ) == "DUSK" ) {
                    valid_times_of_day.push_back( std::make_pair( sunset - 1_hours, sunset + 1_hours ) );
                } else if( ( elem ) == "DAWN" ) {
                    valid_times_of_day.push_back( std::make_pair( sunrise - 1_hours, sunrise + 1_hours ) );
                }
            }

            //If we have any seasons listed, we know to limit by season, and if any season matches this season, we are good to spawn
            if( ( elem ) == "SUMMER" || ( elem ) == "WINTER" || ( elem ) == "SPRING" ||
                ( elem ) == "AUTUMN" ) {
                season_limited = true;
                if( ( season_of_year( calendar::turn ) == SUMMER && ( elem ) == "SUMMER" ) ||
                    ( season_of_year( calendar::turn ) == WINTER && ( elem ) == "WINTER" ) ||
                    ( season_of_year( calendar::turn ) == SPRING && ( elem ) == "SPRING" ) ||
                    ( season_of_year( calendar::turn ) == AUTUMN && ( elem ) == "AUTUMN" ) ) {
                    season_matched = true;
                }
            }
        }

        //Make sure the current time of day is within one of the valid time ranges for this spawn
        bool is_valid_time_of_day = false;
        if( valid_times_of_day.empty() ) {
            //Then it can spawn whenever, since no times were defined
            is_valid_time_of_day = true;
        } else {
            //Otherwise, it's valid if it matches any of the times of day
            for( auto &elem : valid_times_of_day ) {
                if( calendar::turn > elem.first && calendar::turn < elem.second ) {
                    is_valid_time_of_day = true;
                }
            }
        }
        if( !is_valid_time_of_day ) {
            valid_entry = false;
        }

        //If we are limited by season, make sure we matched a season
        if( season_limited && !season_matched ) {
            valid_entry = false;
        }

        //If the entry was valid, check to see if we actually spawn it
        if( valid_entry ) {
            //If the monsters frequency is greater than the spawn_chance, select this spawn rule
            if( it->frequency >= spawn_chance ) {
                if( it->pack_maximum > 1 ) {
                    spawn_details = MonsterGroupResult( it->name, rng( it->pack_minimum, it->pack_maximum ) );
                } else {
                    spawn_details = MonsterGroupResult( it->name, 1 );
                }
                //And if a quantity pointer with remaining value was passed, will modify the external value as a side effect
                //We will reduce it by the spawn rule's cost multiplier
                if( quantity ) {
                    *quantity -= std::max( 1, it->cost_multiplier * spawn_details.pack_size );
                }
                monster_found = true;
                //Otherwise, subtract the frequency from spawn result for the next loop around
            } else {
                spawn_chance -= it->frequency;
            }
        }
    }

    // Force quantity to decrement regardless of whether we found a monster.
    if( quantity && !monster_found ) {
        ( *quantity )--;
    }

    return spawn_details;
}

bool MonsterGroup::IsMonsterInGroup( const mtype_id &mtypeid ) const
{
    if( defaultMonster == mtypeid ) {
        return true;
    }
    for( auto &m : monsters ) {
        if( m.name == mtypeid ) {
            return true;
        }
    }
    return false;
}

bool MonsterGroupManager::IsMonsterInGroup( const mongroup_id &group, const mtype_id &monster )
{
    return group.obj().IsMonsterInGroup( monster );
}

const mongroup_id &MonsterGroupManager::Monster2Group( const mtype_id &monster )
{
    for( auto &g : monsterGroupMap ) {
        if( g.second.IsMonsterInGroup( monster ) ) {
            return g.second.name;
        }
    }
    return mongroup_id::NULL_ID();
}

std::vector<mtype_id> MonsterGroupManager::GetMonstersFromGroup( const mongroup_id &group )
{
    const MonsterGroup &g = group.obj();

    std::vector<mtype_id> monsters;

    monsters.push_back( g.defaultMonster );

    for( auto &elem : g.monsters ) {
        monsters.push_back( elem.name );
    }
    return monsters;
}

bool MonsterGroupManager::isValidMonsterGroup( const mongroup_id &group )
{
    return monsterGroupMap.count( group ) > 0;
}

const MonsterGroup &MonsterGroupManager::GetMonsterGroup( const mongroup_id &group )
{
    const auto it = monsterGroupMap.find( group );
    if( it == monsterGroupMap.end() ) {
        debugmsg( "Unable to get the group '%s'", group.c_str() );
        // Initialize the group with a null-monster, it's ignored while spawning,
        // but it prevents further messages about invalid monster type id
        auto &g = monsterGroupMap[group];
        g.name = group;
        g.defaultMonster = mtype_id::NULL_ID();
        return g;
    } else {
        return it->second;
    }
}

// see item_factory.cpp
extern void add_to_set( std::set<std::string> &s, JsonObject &json, const std::string &name );

void MonsterGroupManager::LoadMonsterBlacklist( JsonObject &jo )
{
    add_to_set( monster_blacklist, jo, "monsters" );
    add_to_set( monster_categories_blacklist, jo, "categories" );
}

void MonsterGroupManager::LoadMonsterWhitelist( JsonObject &jo )
{
    if( jo.has_string( "mode" ) && jo.get_string( "mode" ) == "EXCLUSIVE" ) {
        monster_whitelist_is_exclusive = true;
    }
    add_to_set( monster_whitelist, jo, "monsters" );
    add_to_set( monster_categories_whitelist, jo, "categories" );
}

bool MonsterGroupManager::monster_is_blacklisted( const mtype_id &m )
{
    if( monster_whitelist.count( m.str() ) > 0 ) {
        return false;
    }
    const mtype &mt = m.obj();
    for( const auto &elem : monster_categories_whitelist ) {
        if( mt.categories.count( elem ) > 0 ) {
            return false;
        }
    }
    for( const auto &elem : monster_categories_blacklist ) {
        if( mt.categories.count( elem ) > 0 ) {
            return true;
        }
    }
    if( monster_blacklist.count( m.str() ) > 0 ) {
        return true;
    }
    // Return true if the whitelist mode is exclusive and either whitelist is populated.
    return monster_whitelist_is_exclusive &&
           ( !monster_whitelist.empty() || !monster_categories_whitelist.empty() );
}

void MonsterGroupManager::FinalizeMonsterGroups()
{
    for( auto &mtid : monster_whitelist ) {
        if( !mtype_id( mtid ).is_valid() ) {
            debugmsg( "monster on whitelist %s does not exist", mtid.c_str() );
        }
    }
    for( auto &mtid : monster_blacklist ) {
        if( !mtype_id( mtid ).is_valid() ) {
            debugmsg( "monster on blacklist %s does not exist", mtid.c_str() );
        }
    }
    for( auto &elem : monsterGroupMap ) {
        MonsterGroup &mg = elem.second;
        for( FreqDef::iterator c = mg.monsters.begin(); c != mg.monsters.end(); ) {
            if( MonsterGroupManager::monster_is_blacklisted( c->name ) ) {
                c = mg.monsters.erase( c );
            } else {
                ++c;
            }
        }
        if( MonsterGroupManager::monster_is_blacklisted( mg.defaultMonster ) ) {
            mg.defaultMonster = mtype_id::NULL_ID();
        }
    }
}

void MonsterGroupManager::LoadMonsterGroup( JsonObject &jo )
{
    MonsterGroup g;

    g.name = mongroup_id( jo.get_string( "name" ) );
    g.defaultMonster = mtype_id( jo.get_string( "default" ) );
    if( jo.has_array( "monsters" ) ) {
        JsonArray monarr = jo.get_array( "monsters" );

        while( monarr.has_more() ) {
            JsonObject mon = monarr.next_object();
            const mtype_id name = mtype_id( mon.get_string( "monster" ) );
            int freq = mon.get_int( "freq" );
            int cost = mon.get_int( "cost_multiplier" );
            int pack_min = 1;
            int pack_max = 1;
            if( mon.has_member( "pack_size" ) ) {
                JsonArray packarr = mon.get_array( "pack_size" );
                pack_min = packarr.next_int();
                pack_max = packarr.next_int();
            }
            static const time_duration tdfactor = 1_hours;
            time_duration starts = 0;
            time_duration ends = 0;
            if( mon.has_member( "starts" ) ) {
                if( get_option<float>( "MONSTER_UPGRADE_FACTOR" ) > 0 ) {
                    starts = tdfactor * mon.get_int( "starts" ) * get_option<float>( "MONSTER_UPGRADE_FACTOR" );
                } else {
                    // Default value if the monster upgrade factor is set to 0.0 - off
                    starts = tdfactor * mon.get_int( "starts" );
                }
            }
            if( mon.has_member( "ends" ) ) {
                if( get_option<float>( "MONSTER_UPGRADE_FACTOR" ) > 0 ) {
                    ends = tdfactor * mon.get_int( "ends" ) * get_option<float>( "MONSTER_UPGRADE_FACTOR" );
                } else {
                    // Default value if the monster upgrade factor is set to 0.0 - off
                    ends = tdfactor * mon.get_int( "ends" );
                }
            }
            MonsterGroupEntry new_mon_group = MonsterGroupEntry( name, freq, cost, pack_min, pack_max, starts,
                                              ends );
            if( mon.has_member( "conditions" ) ) {
                JsonArray conditions_arr = mon.get_array( "conditions" );
                while( conditions_arr.has_more() ) {
                    new_mon_group.conditions.push_back( conditions_arr.next_string() );
                }
            }



            g.monsters.push_back( new_mon_group );
        }
    }
    g.replace_monster_group = jo.get_bool( "replace_monster_group", false );
    g.new_monster_group = mongroup_id( jo.get_string( "new_monster_group_id",
                                       mongroup_id::NULL_ID().str() ) );
    assign( jo, "replacement_time", g.monster_group_time, false, 1_days );
    g.is_safe = jo.get_bool( "is_safe", false );

    monsterGroupMap[g.name] = g;
}

void MonsterGroupManager::ClearMonsterGroups()
{
    monsterGroupMap.clear();
    monster_blacklist.clear();
    monster_whitelist.clear();
    monster_whitelist_is_exclusive = false;
    monster_categories_blacklist.clear();
    monster_categories_whitelist.clear();
}

void MonsterGroupManager::check_group_definitions()
{
    for( auto &e : monsterGroupMap ) {
        const MonsterGroup &mg = e.second;
        if( !mg.defaultMonster.is_valid() ) {
            debugmsg( "monster group %s has unknown default monster %s", mg.name.c_str(),
                      mg.defaultMonster.c_str() );
        }
        for( const auto &mge : mg.monsters ) {
            if( !mge.name.is_valid() ) {
                // mon_null should not be valid here
                debugmsg( "monster group %s contains unknown monster %s", mg.name.c_str(), mge.name.c_str() );
            }
        }
    }
}

const mtype_id &MonsterGroupManager::GetRandomMonsterFromGroup( const mongroup_id &group_name )
{
    int spawn_chance = rng( 1, 1000 );
    const auto &group = group_name.obj();
    for( auto it = group.monsters.begin(); it != group.monsters.end(); ++it ) {
        if( it->frequency >= spawn_chance ) {
            return it->name;
        } else {
            spawn_chance -= it->frequency;
        }
    }

    return group.defaultMonster;
}
