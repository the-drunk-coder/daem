#include "RtAudio.h"
#include <iostream>

// this header contains some convenience methods
// to faciliate the use of RtAudio !

// space to buffer the de-interleaved sample data ... maybe there's a better way to do this in situ ?
void *in_block_deinterleaved;
void *out_block_deinterleaved;

// PARAMS FOR CONVENIENCE !
struct AudioInitParams {
  AudioInitParams(){
    input_channels = 2;
    output_channels = 2;
    input_device = -1;
    output_device = -1;
    process_input = true;
    buffer_size = 1024;
    samplerate = 44100;
    stream_format = RTAUDIO_FLOAT64;
  }
  unsigned int samplerate;
  int input_channels;
  int output_channels;
  int input_device;
  int output_device;
  bool process_input;
  unsigned int buffer_size;
  RtAudioFormat stream_format;
};

/*
struct SimpleAudio {
  RtAudio* adac;
  AudioInitParams* params;
};
*/

struct AudioData {
  AudioInitParams* params;
};

// More efficient way to do this ?
template<typename SAMPLE_TYPE>
void de_interleave_block(SAMPLE_TYPE *interleaved_samples, SAMPLE_TYPE **channel_samples, AudioInitParams* params) {
  //std::cout << "de_interleave " << params->buffer_size << std::endl;
  //unsigned long int sample_count = 0;
  unsigned int frame_index = 0;
  for(int i = 0; i < params->buffer_size; i++) {
    for(int c = 0; c < params->input_channels; c++) {
      channel_samples[c][i] = interleaved_samples[frame_index + c];
      //sample_count++;
    }
    frame_index += params->output_channels;
  }
  //std::cout << "SAMPLES READ: " << sample_count << std::endl;
}

template<typename SAMPLE_TYPE>
void interleave_block(SAMPLE_TYPE **channel_samples, SAMPLE_TYPE *interleaved_samples, AudioInitParams* params) {
  //std::cout << "interleave " << params->buffer_size << std::endl;
  //unsigned long int sample_count = 0;
  unsigned int frame_index = 0;
  for(int i = 0; i < params->buffer_size; i++) {
    for(int c = 0; c < params->output_channels; c++) {
      interleaved_samples[frame_index + c] = channel_samples[c][i];
      //sample_count++;
    }
    frame_index += params->output_channels;
  }
  //std::cout << "SAMPLES WRITTEN: " << sample_count << std::endl;
}

template<typename READ_TYPE, typename WRITE_TYPE, void (*AUDIO_CALLBACK)(READ_TYPE**, WRITE_TYPE**, AudioInitParams*, double, void*)>
int audio_callback_internal(void *output_buffer, void *input_buffer,
  unsigned int buffer_frames,
  double stream_time,
  RtAudioStreamStatus status,
  void *data)
  {
    //std::cout << "BLOCK PROCESSING !! "<< std::endl;
    if ( status ) { std::cout << "Stream over/underflow detected." << std::endl; }

    AudioData* ad = reinterpret_cast<AudioData *>(data);
    AudioInitParams* params = ad->params;
    /*
    std::cout << params->buffer_size << std::endl;
    std::cout << buffer_frames << std::endl;
    std::cout << stream_time << std::endl;
    std::cout << params->samplerate << std::endl;
    std::cout << params->input_channels<< std::endl;
    */

    if(params->process_input){
      de_interleave_block((READ_TYPE *) input_buffer, (READ_TYPE **) in_block_deinterleaved, params);
    }
    AUDIO_CALLBACK((READ_TYPE**) in_block_deinterleaved, (WRITE_TYPE**) out_block_deinterleaved, params, stream_time, data);
    interleave_block((WRITE_TYPE **) out_block_deinterleaved, (WRITE_TYPE *) output_buffer, params);

    return 0;
}


template<typename READ_TYPE = double, typename WRITE_TYPE = READ_TYPE, void (AUDIO_CALLBACK)(READ_TYPE **input, WRITE_TYPE **output, AudioInitParams* params, double stream_time, void* data)>
void init_audio(RtAudio* adac, AudioInitParams* params, AudioData* data){
  RtAudio::StreamParameters iParams, oParams;
  //std::cout << "INITIALIZING PARAMS" << std::endl;
  // Output Params
  if(params->process_input){
    // Input Params
    if(params->input_device == -1){
      // first available device, if -1!
      params->input_device = adac->getDefaultInputDevice();
    }
    iParams.deviceId = params->input_device;
    iParams.nChannels = params->input_channels;
  }

  if(params->output_device == -1){
    // first available device, if -1!
    params->output_device = adac->getDefaultOutputDevice();
  }
  oParams.deviceId = params->output_device; // first available device
  oParams.nChannels = params->output_channels;

  if(data == NULL){
    data = new AudioData();
  }

  data->params = params;
  //std::cout << "INITIALIZING ADAC" << std::endl;
  // Intialize adac
  try {
    adac->openStream(&oParams, &iParams, params->stream_format,
      params->samplerate, &params->buffer_size,
      &audio_callback_internal<READ_TYPE, WRITE_TYPE, AUDIO_CALLBACK>, (void*) data);

  } catch ( RtAudioError& e ) {
    e.printMessage();
      if ( adac->isStreamOpen() ) adac->closeStream();
    exit( 0 );
  }


  // init blocks for de-interleaved audio here, as only now the blocksize can be assured ...
  READ_TYPE** in_deinterleave_space = new READ_TYPE*[params->input_channels];
  //std::cout << "MEM IN " << params->buffer_size << std::endl;
  for(int i = 0; i < params->input_channels; i++){
    in_deinterleave_space[i] = new READ_TYPE[params->buffer_size];
  }
  in_block_deinterleaved = (void *) in_deinterleave_space;

  WRITE_TYPE** out_deinterleave_space = new WRITE_TYPE*[params->input_channels];
  for(int i = 0; i < params->output_channels; i++){
    out_deinterleave_space[i] = new WRITE_TYPE[params->buffer_size];
  }
  out_block_deinterleaved = (void *) out_deinterleave_space;

  try {
    adac->startStream();
  } catch ( RtAudioError& e ) {
    e.printMessage();
    if ( adac->isStreamOpen() ) adac->closeStream();
    exit( 0 );
  }

};

template<typename READ_TYPE = double, typename WRITE_TYPE = READ_TYPE>
void stop_audio(RtAudio* adac, AudioInitParams* params) {

  try {
    adac->stopStream();
  }
  catch ( RtAudioError& e ) {
    e.printMessage();
    if ( adac->isStreamOpen() ) adac->closeStream();
  }

  // cleanup buffers
  READ_TYPE** del_in_block = (READ_TYPE**) in_block_deinterleaved;
  for(int i = 0; i < params->input_channels; i++){
    delete del_in_block[i];
  }
  delete del_in_block;

  WRITE_TYPE** del_out_block = (WRITE_TYPE**) out_block_deinterleaved;
  for(int i = 0; i < params->output_channels; i++){
    delete del_out_block[i];
  }
  delete del_out_block;
};
