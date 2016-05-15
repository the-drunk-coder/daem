#include <iostream>
#include <algorithm>
#include <iterator>
#include <boost/program_options.hpp>
#include <boost/algorithm/string.hpp>
#include <boost/lockfree/spsc_queue.hpp>
#include "simple_rt_wrap.h"
#include "getch.h"

namespace po = boost::program_options;
namespace lfree = boost::lockfree;

// A helper function to simplify the main part (from po example ...)
template <class T>
std::ostream &operator<<(std::ostream &os, const std::vector<T> &v) {
  std::copy(v.begin(), v.end(), std::ostream_iterator<T>(os, " "));
  return os;
}

// effects flags, to indicate which effect should be switched on or off
enum EFLAG { DELAY, HALL, FILTER };

// effects commands
namespace COMMAND {
  enum COMMAND {
    DELAY_ON,
    DELAY_OFF,
    HALL_ON,
    HALL_OFF,    
    FILTER_ON,
    FILTER_OFF,
    FILTER_MODE_CHANGE,
    ALL_OFF
  };
}

// map enable nicer type output ...
std::map <EFLAG, std::string> eflag_strings {
  { DELAY, "delay" },
    { HALL, "hall" },
      { FILTER, "filter" }
};

// custom stream to extract enum from command line
std::istream &operator>>(std::istream &in, EFLAG &eflag) {
  std::string token;
  in >> token;

  boost::to_upper(token);

  if (token == "HALL") {
    eflag = HALL;
  } else if (token == "DELAY") {
    eflag = DELAY;
  } else if (token == "FILTER") {
    eflag = FILTER;
  }

  return in;
}

// naive state variable filter, modeled after DAFx Chapter 2
struct state_variable_filter {
  enum FMODE {HP, BP, LP};
  
  state_variable_filter(){
    update(5000, 2, 44100, LP);
  }

  state_variable_filter(double frequency, double q, int samplerate, FMODE mode){
    update(frequency, q, samplerate, mode);
  }

  void update(double frequency, double q, int samplerate, FMODE mode) {
    q1 = 1.0 / q;
    f1 = 2 * sin(M_PI * frequency / samplerate);
    del_lp = 0.0;
    del_bp = 0.0;
    hp = 0.0;
    lp = 0.0;
    bp = 0.0;
    this->mode = mode;
  }
  
  FMODE mode;

  // parameters
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

  void process(double& sample){
    calculate(sample);   
    if(mode == LP){
      sample = lp;
    } else if (mode == HP) {
      sample = hp;
    } else if (mode == BP) {
      sample = bp;
    }   
  }
};

// command container to exchange information between main- and audio-thread
struct command_container {
  COMMAND::COMMAND cmd;
  EFLAG effect;
  bool new_effect_state;
  state_variable_filter::FMODE new_filter_mode;
};

// a simple filterbank consisting of several state-variable filters
struct filterbank {
  filterbank(int channels, int samplerate, double lowcut, double hicut, int bands) {
    this->bands = bands;
    this->channels = channels;
    
    fbank = new state_variable_filter[channels * bands];
    
    for(int ch = 0; ch < channels; ch++) {
      
      fbank[ch * bands].update(lowcut, 1.5, samplerate, state_variable_filter::HP);
      for(int b = 1; b < bands-1; b++) {
	fbank[(ch * bands) + b].update(lowcut + (b * ((hicut - lowcut) / bands)) , 1.5, samplerate, state_variable_filter::BP);
      }
      fbank[(ch * bands) + bands-1].update(hicut, 1.5, samplerate, state_variable_filter::LP);

    }

    fmask = new bool[bands];
    for(int b = 0; b < bands; b++) {
      fmask[b] = false;
    }
  }

  ~filterbank() {
    delete [] fbank;
    delete [] fmask;
  }

  int bands;
  int channels;
  state_variable_filter* fbank;
  bool* fmask;

  double apply(int channel, double& sample) {    
    for(int b = 0; b < bands; b++){
      if(fmask[b]){	
	fbank[(channel * bands) + b].process(sample);
      }
    }
    return sample;
  }

  void toggle_band(int band) {
    if(fmask[band]) {
      fmask[band] = false;
    } else {
      fmask[band] = true;
    } 
   }
};

// simple delay line, parametrised by samples
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

  double get_delayed_sample(int channel) {
    return buffer[channel][delay_counter];
  }

  void put_next(int channel, double sample) {
    buffer[channel][delay_counter] = sample;
    delay_counter++;
    if(delay_counter >= delay_time){
      delay_counter = 0;
    }
  }
  
  double** buffer;
  int delay_counter;
  int delay_time;
  int delay_channels;
};

// parameters for audio callback function
struct play_params : public AudioData {
  AudioInitParams* params;
  delay_line* dline;
  filterbank* fbank; 
  std::map<EFLAG, bool>* fxmask;
  lfree::spsc_queue<command_container>* cmd_queue;
};

/*
 *
 * THE AUDIO CALLBACK FUNCTION !!!
 *
 */
void audio_callback(double **input, double **output, AudioInitParams* a_params, double stream_time, void* data) {

  play_params* p_params = reinterpret_cast<play_params*>(data);
  delay_line* line = p_params->dline;
  filterbank* fbank = p_params->fbank;
  lfree::spsc_queue<command_container>* cmd_queue = p_params->cmd_queue;
  std::map<EFLAG, bool>* fxmask = p_params->fxmask;
    
  // handle commands
  command_container cont;
  while (cmd_queue->pop(&cont)) {
    if (cont.cmd == COMMAND::DELAY_ON) {
      if (fxmask->at(EFLAG::HALL)) {
	fxmask->at(EFLAG::HALL) = false;
      }
      fxmask->at(EFLAG::DELAY) = true;
    } else if (cont.cmd == COMMAND::DELAY_OFF) {      
       fxmask->at(EFLAG::DELAY) = false; 
    } else if (cont.cmd == COMMAND::HALL_ON) {
      if (fxmask->at(EFLAG::DELAY)) {
	fxmask->at(EFLAG::DELAY) = false;
      }
      fxmask->at(EFLAG::HALL) = true;
    } else if (cont.cmd == COMMAND::HALL_OFF) {
      fxmask->at(EFLAG::HALL) = false; 
    } else if (cont.cmd == COMMAND::FILTER_ON) {
      fxmask->at(EFLAG::FILTER) = true; 
    } else if (cont.cmd == COMMAND::FILTER_OFF) {
      fxmask->at(EFLAG::FILTER) = false; 
    } else if (cont.cmd == COMMAND::FILTER_MODE_CHANGE) {
      //tbd
    } else if (cont.cmd == COMMAND::ALL_OFF) {
      fxmask->at(EFLAG::FILTER) = false;
      fxmask->at(EFLAG::DELAY) = false;
      fxmask->at(EFLAG::HALL) = false;      
    }
  }
  
  for(int channel = 0; channel < a_params->output_channels; channel++) {
    for(int sample = 0; sample < a_params->buffer_size; sample++) {
    
       double current_sample = input[channel][sample];

       if (fxmask->at(EFLAG::FILTER)) {	 
	 fbank->apply(channel, current_sample);
	}
       
       if (fxmask->at(EFLAG::DELAY) || fxmask->at(EFLAG::HALL)) {
	 double delayed_sample = line->get_delayed_sample(channel);        
	 double output_sample = current_sample * 0.5 + delayed_sample * 0.5;
	 if (fxmask->at(EFLAG::DELAY)) {
	   line->put_next(channel, current_sample);
	 } else if (fxmask->at(EFLAG::HALL)) {
	   line->put_next(channel, output_sample);
	 }
	 output[channel][sample] = output_sample;
       } else {
	 output[channel][sample] = current_sample;
       }
    }
  }
}
     
// initialize the command line parameter parser
po::options_description init_opts(int ac, char *av[], po::variables_map *vm,
                                  play_params *params) {

  po::options_description desc("daem Parameters!");
  desc.add_options()
    ("help", "Display this help!")    
    ("time", po::value<int>(), "Delay/Echo time the program will start with!")
    ;
  // ----- end options ... what kind of syntax is this ??

  po::store(po::command_line_parser(ac, av).options(desc).run(),*vm);
  po::notify(*vm);

  return desc;
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
  p_params.dline = &dline;

  std::map<EFLAG, bool> fxmask;
  fxmask.insert(std::pair<EFLAG, bool>(EFLAG::DELAY, true));
  fxmask.insert(std::pair<EFLAG, bool>(EFLAG::HALL, false));
  fxmask.insert(std::pair<EFLAG, bool>(EFLAG::FILTER, false));
  p_params.fxmask = &fxmask;

  lfree::spsc_queue<command_container> cmd_queue(10);
  p_params.cmd_queue = &cmd_queue;
  
  RtAudio adac;
  AudioInitParams a_params;

  filterbank fbank(2, 44100, 100, 10000, 10);
  p_params.fbank = &fbank;
  
  init_audio<double, double, audio_callback>(&adac, &a_params, &p_params);

  std::cout << "Press 'q' to exit!" << std::endl;
  
  char input;
  
  while((input = getch()) != 'q') {
    command_container cont;
    switch(input){
    case '1':
    case '2':
    case '3':
    case '4':
    case '5':
    case '6':
    case '7':
    case '8':
    case '9':
    case '0':
      // stupidly simple, but it seems to work ...
      if (input == '0'){input = 9;}
      else {input -= 49;}
      fbank.toggle_band(input);
      if(fxmask.at(EFLAG::FILTER)){
	std::cout << "Filter bands: ";
	for(int i = 0; i < fbank.bands; i++){
	  std::cout << "[" << fbank.fmask[i] << "] ";
	}
      }
      std::cout << std::endl;
      break;
    case 'f':
      if (fxmask.at(EFLAG::FILTER)) {
	std::cout << "filter off" << std::endl;
	cont.cmd = COMMAND::FILTER_OFF;
      } else {
	std::cout << "filter on" << std::endl;		
	cont.cmd = COMMAND::FILTER_ON;
      }      
    }
    cmd_queue.push(cont);
  }
 
  stop_audio<>(&adac, &a_params);

  return 0;
}
