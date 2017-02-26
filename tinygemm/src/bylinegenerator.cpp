#include <string>
#include <sstream>
#include <iostream>

#include <tinygemm/bylinegenerator.hpp>
#include <tinygemm/tinygemmerror.hpp>

#include <tinygemm/prepgenerator.hpp>


/* TODO : (more for alpha kernel : consider using the keyword restrict more liberally). Some initial thought suggests to me that it more be more approprialtely pult on c. where did the use og restrict inherit from anywayt??? TODO TODO TODO */

/* TODO : could interwoven be faster ? */

namespace tinygemm{
namespace bylinegen{

void ByLineGenerator::setup(){

  setup_additional();
  n_full_work_items_per_line = gg.get_coal(matrixchar) / get_work_per_thread();
  n_work_items_per_line = n_full_work_items_per_line + (gg.get_coal(matrixchar) % get_work_per_thread() != 0);
  n_full_work_items = n_full_work_items_per_line*gg.get_uncoal(matrixchar);
  n_work_items = n_work_items_per_line*gg.get_uncoal(matrixchar);
  start_in_coal_last_work_item = get_work_per_thread()*n_full_work_items_per_line;
  work_for_last_item_in_coal = gg.get_coal(matrixchar) % get_work_per_thread();
  set_usage_from_matrixchar();
}


ByLineGenerator::ByLineGenerator(const tinygemm::hyperparams::HyperParams & hp_,  const tinygemm::TinyGemmGeometry & gg_, const tinygemm::derivedparams::DerivedParams & dp_, std::string type_): prepgen::PrepGenerator(hp_, gg_, dp_, type_){}


void ByLineGenerator::append_description_string(std::stringstream & ss){
  ss <<  description_string;
}





void ByLineGenerator::append_how_definitions(std::stringstream & ss){
  ss << 
R"(/* The number of values from C which each non-edge work-item will scale by beta */
#define WORK_PER_THREAD  )" << get_work_per_thread()  << R"(
/* The number of work items per work group
 * TODO : generalise for vega support */
#define N_WORK_ITEMS_PER_GROUP )" << get_local_work_size() << "\n\n";
}



void ByLineGenerator::append_derived_definitions(std::stringstream & ss){

  ss << "/*      each (full) work item will process WORK_PER_THREAD elements in the coalesced direction, */ \n";
  ss << "/*      so the number of work items per coalesced line is DIM_COAL / WORK_PER_THREAD */ \n"; 
  ss << "#define N_FULL_WORK_ITEMS_PER_LINE " << n_full_work_items_per_line << "\n";
  ss << "/*      including the possible final tail thread, */\n";
  ss << "/*      there are N_FULL_WORK_ITEMS_PER_LINE + (DIM_COAL % WORK_PER_THREAD != 0) */ \n";
  ss << "#define N_WORK_ITEMS_PER_LINE " << n_work_items_per_line << "\n";
  ss << "/*      in total there are N_FULL_WORK_ITEMS_PER_LINE * DIM_UNCOAL full work items, */ \n";

  ss << "#define N_FULL_WORK_ITEMS " << n_full_work_items << "\n";
  ss << "/*      and a grand total of N_WORK_ITEMS_PER_LINE * DIM_UNCOAL work items. */ \n";
  ss << "#define N_WORK_ITEMS " << n_work_items << "\n";  
  ss << "/*      tail work items start at WORK_PER_THREAD * N_FULL_WORK_ITEMS_PER_LINE in the coalesced direction,  */\n";
  ss << "#define START_IN_COAL_LAST_WORK_ITEM " << start_in_coal_last_work_item <<  "\n";
  ss << "/*      and process DIM_COAL % WORK_PER_THREAD elements of c */\n";

  ss << "#define WORK_FOR_LAST_ITEM_IN_COAL " << work_for_last_item_in_coal << "\n";
  ss << "/*      the target stride between lines, derived from hp and gg (see DerivedParams) */\n";

  append_derived_definitions_additional(ss);
}



size_t ByLineGenerator::get_n_work_groups(){
  size_t number_of_workgroups = (n_work_items / get_local_work_size()) + ((n_work_items % get_local_work_size()) != 0);
  return number_of_workgroups;
}
  

void ByLineGenerator::append_setup_coordinates(std::stringstream & ss){
  
    ss << R"(
    
    
/* setting up where this thread works */
unsigned group_id = get_group_id(0);
unsigned local_id = get_local_id(0);
unsigned global_id = group_id*N_WORK_ITEMS_PER_GROUP + local_id; 

unsigned start_uncoal = 0;
unsigned start_coal = 0;

bool is_in_full_zone = (global_id < N_FULL_WORK_ITEMS);
if (is_in_full_zone){   
start_uncoal = global_id / N_FULL_WORK_ITEMS_PER_LINE;
start_coal = WORK_PER_THREAD * (global_id % N_FULL_WORK_ITEMS_PER_LINE);
}

else if (global_id < N_WORK_ITEMS){
start_uncoal = (global_id - N_FULL_WORK_ITEMS)% DIM_UNCOAL;
start_coal = START_IN_COAL_LAST_WORK_ITEM;
}

)";
}

void ByLineGenerator::append_positioning_x_string(std::stringstream & ss){

ss << "\n\n/* moving the " << matrixchar << " pointer to the first element to process */\n";
ss << matrixchar << " += " << matrixchar << "_offset;\n";
ss << matrixchar << " += start_uncoal * LD" << MATRIXCHAR << ";\n";
ss << matrixchar << " += start_coal;\n";

}


void ByLineGenerator::append_inner_work(std::stringstream & ss){
  ss << inner_work_string;  
}

void ByLineGenerator::append_work_string(std::stringstream & ss){
  
  
  ss << 
R"(
if (is_in_full_zone){
#pragma unroll WORK_PER_THREAD
for (unsigned i = 0; i < WORK_PER_THREAD; ++i){  )";
  append_inner_work(ss);
  ss << "\n}\n}\n";

  ss << R"(
else if (global_id < N_WORK_ITEMS){
for (unsigned i = 0; i < WORK_FOR_LAST_ITEM_IN_COAL; ++i){  )";
append_inner_work(ss);
  ss << "\n}\n}\n";
 
}


void ByLineGenerator::append_positioning_w_string(std::stringstream & ss){

  ss << R"(

/* moving the y pointer to the first element to process */
w += GLOBAL_OFFSET_W;
w += w_offset;
w += start_uncoal * LDW;
w += start_coal;
)";
}


KernelString ByLineGenerator::get_kernelstring(){




  std::stringstream ss;

  ss << get_time_string();
  append_description_string(ss);

  ss << "\n\n" << get_what_string() << "\n";
  append_basic_what_definitions(ss);

  ss << get_how_string() << "\n";
  append_how_definitions(ss);

  ss << get_derived_string() << "\n";
  append_derived_definitions(ss);
  
  ss << "\n\n" << "__attribute__((reqd_work_group_size(N_WORK_ITEMS_PER_GROUP,1,1)))" << "\n";
  ss << "__kernel void ";
  
  ss << kernelname;
  append_parameter_list_from_usage(ss);

  ss << "{";
  
  append_setup_coordinates(ss);
  append_positioning_x_string(ss);

  if (matrixchar == 'a' || matrixchar == 'b'){
    append_positioning_w_string(ss);
  }
  
  append_work_string(ss);
  
  ss << "\n}\n\n\n";

  return {{uses_a, uses_b, uses_c, uses_workspace, uses_alpha, uses_beta}, ss.str(), kernelname, get_global_work_size(), get_local_work_size()};

}




}
}
