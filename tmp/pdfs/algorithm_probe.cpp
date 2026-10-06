#include "hexa_udon/optimizer/optimizer.hpp"
#include <chrono>
#include <iostream>
using namespace hexa_udon;
int main() {
 std::vector<core::Terrain> cells(256,core::Terrain::Plain);
 cells[240]=core::Terrain::Mountain; cells[241]=core::Terrain::Pond; cells[242]=core::Terrain::Road;
 auto map = core::MapDefinition::create(16,16,cells).value();
 std::vector<core::Spot> spots{{0,{1},4}};
 for (int row=12;row<=15;++row) for (int col=12;col<=15 && spots.size()<16;++col)
   spots.push_back({static_cast<int>(spots.size()%14),{row*16+col},1});
 core::MatchConfig match{1,{60,60,60,60},{32,32,32,32},map,spots,{{0},{2},{17},{255}},32,2,2,4};
 core::DailyState daily{100,0,{{core::AgentKind::Patrol,{0},32},{core::AgentKind::Patrol,{2},32},{core::AgentKind::Patrol,{17},32},{core::AgentKind::Supply,{255},0}},{},{{{242},core::RoadStatus::Smooth}}};
 std::cout<<"validation match="<<core::validate(match).size()<<" daily="<<core::validate(daily,match).size()<<"\n";
 simulator::MatchProgress progress;
 auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
 auto greedy=planner::make_greedy_plan({match,daily,progress},{},deadline);
 auto refuel=planner::make_refuel_plan({match,daily,progress},greedy.value(),{},deadline);
 auto optimized=optimizer::optimize({match,daily,progress},greedy.value(),refuel.value(),{},deadline);
 auto manual=simulator::simulate_day({map,match.spots,32,32,daily.own_agents,daily.traffic},simulator::RawDayActionPlan{{2,-30},{5,-30},{1,-30},{-32}});
 std::cout<<"stock4 greedy="<<greedy.value().score.total_bowls<<" refuel="<<refuel.value().score.total_bowls<<" optimized="<<optimized.value().score.total_bowls<<" valid_manual="<<manual.value().total_balls<<"\n";
 auto sim=manual.value();
 auto r1=planner::daily_readiness(match,daily,sim,{});
 auto r2=planner::daily_readiness(match,daily,sim,{{0},{0},{0},{}});
 std::cout<<"identical_plan_readiness empty_visits="<<r1.uncollected_spot_reachability<<" explicit_visits="<<r2.uncollected_spot_reachability<<"\n";
 auto small=sim;
 small.end_agents={{core::AgentKind::Patrol,{0},1}};
 match.spots={{0,{17},4}};
 auto hex=planner::daily_readiness(match,daily,small,{});
 std::cout<<"hex_adjacent="<<(map.neighbor({0},core::Direction::LowerRight)==core::CellIndex{17})<<" actual_plain_fuel=1 estimated_reachability="<<hex.uncollected_spot_reachability<<"\n";
}
