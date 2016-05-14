#include <iostream>
#include <algorithm>
#include <iterator>
#include <boost/program_options.hpp>
#include <boost/algorithm/string.hpp>
#include "simple_rt_wrap.h"
#include "getch.h"

namespace po = boost::program_options;

// A helper function to simplify the main part (from po example ...)
template <class T>
std::ostream &operator<<(std::ostream &os, const std::vector<T> &v) {
  std::copy(v.begin(), v.end(), std::ostream_iterator<T>(os, " "));
  return os;
}

// format enum
enum PMODE { DELAY, HALL };


// map enable nicer type output ...
std::map <PMODE, std::string> pmode_strings {
  { DELAY, "delay" },
  { HALL, "hall" },  
};

// custom stream to extract enum from command line
std::istream &operator>>(std::istream &in, PMODE &pmode) {
  std::string token;
  in >> token;

  boost::to_upper(token);

  if (token == "HALL") {
    pmode = HALL;
  } else if (token == "DELAY") {
    pmode = DELAY;
  }
  
  return in;
}

// naive state variable filter, modeled after DAFx Chapter 2
struct state_variable_filter {
  state_variable_filter(double frequency, double q, int samplerate){
    q1 = 1.0 / q;
    f1 = 2 * sin(M_PI * frequency / samplerate);
    del_lp = 0.0;
    del_bp = 0.0;
    hp = 0.0;
    lp = 0.0;
    bp = 0.0;
  }

  //double frequency;
  //double q;
  double q1;
  double f1;

  //delays
  double del_lp;
  double del_bp;

  // current
  double hp;
  double bp;
  double lp;

  void calculate(double sample){
    hp = sample - del_lp - q1 * del_bp;
    bp = f1 * hp + del_bp;
    lp = f1 * bp + del_lp;
    del_bp = bp;
    del_lp = lp;
  }
};


struct delay_line {    
  delay_line(int time, int channels){
    delay_counter = 0;
    delay_time = time;
    delay_channels = channels;
    buffer = new double*[channels];
    
    for(int i = 0; i < channels; i++){
      buffer[i] = new double[time];
      for(int j = 0; j < time; j++){
        buffer[i][j] = 0.0;
      }
    }
  }

  ~delay_line(){
    for(int i = 0; i < delay_channels; i++){
      delete[] buffer[i];
    }
    delete[] buffer;
  }
  
  double** buffer;
  int delay_counter;
  int delay_time;
  int delay_channels;
};

struct play_params : public AudioData {
  AudioInitParams* params;
  delay_line *dline;
  PMODE pmode;
};



// initialize the command line parameter parser
po::options_description init_opts(int ac, char *av[], po::variables_map *vm,
                                  play_params *params) {

  po::options_description desc("daem Parameters!");
  desc.add_options()
    ("help", "Display this help!")
    ("mode", po::value<PMODE>(&(*params).pmode)->default_value(DELAY), "Mode - echo or delay!")    
    ("time", po::value<int>(), "Repeat every sample n times!")
    ;
  // ----- end options ... what kind of syntax is this ??

  po::store(po::command_line_parser(ac, av).options(desc).run(),*vm);
  po::notify(*vm);

  return desc;
}

/*
 *
 * THE AUDIO CALLBACK FUNCTION !!!
 *
 */
void audio_callback(double **input, double **output, AudioInitParams* a_params, double stream_time, void* data)
{

  play_params *p_params = reinterpret_cast<play_params*>(data);
  delay_line *line = p_params->dline;
  
  for(int channel = 0; channel < a_params->output_channels; channel++){
    for(int sample = 0; sample < a_params->buffer_size; sample++){

       //output[channel][sample] = input[channel][sample];

       double current_sample = input[channel][sample];
       double delayed_sample = line->buffer[channel][line->delay_counter];
       double output_sample = output[channel][sample] = current_sample * 0.5 + delayed_sample * 0.5;

       if(p_params->pmode == PMODE::DELAY){
         line->buffer[channel][line->delay_counter] = current_sample;
       } else if (p_params->pmode == PMODE::HALL) {
	 line->buffer[channel][line->delay_counter] = output_sample;
       }
       
       line->delay_counter++;
       if(line->delay_counter >= line->delay_time){
         line->delay_counter = 0;
       }
    }
  }
}

int main(int ac, char *av[]){

  std::cout << "\n~~ daem - create noise abusing internal laptop feedback! ~~\n" << std::endl;

  play_params p_params;

  // initialize command line options
  po::variables_map vm;
  po::options_description desc = init_opts(ac, av, &vm, &p_params);

  int delay_time = 22050;
  if (vm.count("time")) {
    delay_time = vm["time"].as<int>();
  }

  delay_line dline(delay_time, 2);
  p_params.dline  = &dline;
  
  RtAudio adac;
  AudioInitParams a_params;
  
  init_audio<double, double, audio_callback>(&adac, &a_params, &p_params);

  std::cout << "Press 'q' to exit!" << std::endl;

  
  char input;
  while((input = getch()) != 'q')
 
  stop_audio<>(&adac, &a_params);

  return 0;
}
