#include "gpuRadialSensePrepGadget.h"
#include "Gadgetron.h"
#include "GadgetIsmrmrdReadWrite.h"
#include "cuNonCartesianSenseOperator.h"
#include "SenseJob.h"
#include "cuNDArray_elemwise.h"
#include "cuNDArray_utils.h"
#include "vector_td_operators.h"
#include "b1_map.h"
#include "GPUTimer.h"
#include "check_CUDA.h"
#include "radial_utilities.h"
#include "hoNDArray_fileio.h"

#include <algorithm>
#include <vector>
#include <cmath>

namespace Gadgetron{

  gpuRadialSensePrepGadget::gpuRadialSensePrepGadget()
    : slices_(-1)
    , sets_(-1)
    , device_number_(-1)
    , mode_(-1)
    , samples_per_profile_(-1)
  {
    // Set some default values in case the config does not contain a specification
    //

    set_parameter(std::string("mode").c_str(), "0");
    set_parameter(std::string("deviceno").c_str(), "0");
    set_parameter(std::string("buffer_length_in_rotations").c_str(), "1");
    set_parameter(std::string("buffer_using_solver").c_str(), "false");
    set_parameter(std::string("buffer_convolution_kernel_width").c_str(), "5.5");
    set_parameter(std::string("buffer_convolution_oversampling_factor").c_str(), "1.25");
    set_parameter(std::string("rotations_per_reconstruction").c_str(), "0");
    set_parameter(std::string("reconstruction_os_factor_x").c_str(), "1.0");
    set_parameter(std::string("reconstruction_os_factor_y").c_str(), "1.0");
  }
  
  gpuRadialSensePrepGadget::~gpuRadialSensePrepGadget() {}
  
  int gpuRadialSensePrepGadget::process_config(ACE_Message_Block* mb)
  {
    //GADGET_DEBUG1("gpuRadialSensePrepGadget::process_config\n");

    // Get configuration values from config file
    //

    mode_ = get_int_value(std::string("mode").c_str());
    device_number_ = get_int_value(std::string("deviceno").c_str());
    rotations_per_reconstruction_ = get_int_value(std::string("rotations_per_reconstruction").c_str());
    buffer_length_in_rotations_ = get_int_value(std::string("buffer_length_in_rotations").c_str());
    buffer_using_solver_ = get_bool_value(std::string("buffer_using_solver").c_str());
    output_timing_ = get_bool_value(std::string("output_timing").c_str());

    // Currently there are some restrictions on the allowed sliding window configurations
    //
    
    sliding_window_profiles_ = get_int_value(std::string("sliding_window_profiles").c_str());
    sliding_window_rotations_ = get_int_value(std::string("sliding_window_rotations").c_str());

    if( sliding_window_profiles_>0 && sliding_window_rotations_>0 ){
      GADGET_DEBUG1( "Error: Sliding window reconstruction is not yet supported for both profiles and frames simultaneously.\n" );
      return GADGET_FAIL;
    }

    if( sliding_window_profiles_>0 && rotations_per_reconstruction_>0 ){
      GADGET_DEBUG1( "Error: Sliding window reconstruction over profiles is not yet supported for multiframe reconstructions.\n" );
      return GADGET_FAIL;
    }
    
    if( sliding_window_rotations_ > 0 && sliding_window_rotations_ >= rotations_per_reconstruction_ ){
      GADGET_DEBUG1( "Error: Illegal sliding window configuration.\n" );
      return GADGET_FAIL;
    }

    // Setup and validate device configuration
    //

    int number_of_devices;
    if (cudaGetDeviceCount(&number_of_devices)!= cudaSuccess) {
      GADGET_DEBUG1( "Error: unable to query number of CUDA devices.\n" );
      return GADGET_FAIL;
    }

    if (number_of_devices == 0) {
      GADGET_DEBUG1( "Error: No available CUDA devices.\n" );
      return GADGET_FAIL;
    }

    if (device_number_ >= number_of_devices) {
      GADGET_DEBUG2("Adjusting device number from %d to %d\n", device_number_,  (device_number_%number_of_devices));
      device_number_ = (device_number_%number_of_devices);
    }

    if (cudaSetDevice(device_number_)!= cudaSuccess) {
      GADGET_DEBUG1( "Error: unable to set CUDA device.\n" );
      return GADGET_FAIL;
    }

    cudaDeviceProp deviceProp;
    if( cudaGetDeviceProperties( &deviceProp, device_number_ ) != cudaSuccess) {
      GADGET_DEBUG1( "Error: unable to query device properties.\n" );
      return GADGET_FAIL;
    }
    
    unsigned int warp_size = deviceProp.warpSize;

    // Convolution kernel width and oversampling ratio (for the buffer)
    //

    kernel_width_ = get_double_value(std::string("buffer_convolution_kernel_width").c_str());
    oversampling_factor_ = get_double_value(std::string("buffer_convolution_oversampling_factor").c_str());

    // Get the Ismrmrd header
    //

    boost::shared_ptr<ISMRMRD::ismrmrdHeader> cfg = parseIsmrmrdXMLHeader(std::string(mb->rd_ptr()));
    
    if( cfg.get() == 0x0 ){
      GADGET_DEBUG1("Unable to parse Ismrmrd header\n");
      return GADGET_FAIL;
    }

    ISMRMRD::ismrmrdHeader::encoding_sequence e_seq = cfg->encoding();

    if (e_seq.size() != 1) {
      GADGET_DEBUG2("Number of encoding spaces: %d\n", e_seq.size());
      GADGET_DEBUG1("This Gadget only supports one encoding space\n");
      return GADGET_FAIL;
    }
    
    //ISMRMRD::encodingSpaceType e_space = (*e_seq.begin()).encodedSpace();
    ISMRMRD::encodingSpaceType r_space = (*e_seq.begin()).reconSpace();
    ISMRMRD::encodingLimitsType e_limits = (*e_seq.begin()).encodingLimits();

    // Matrix sizes (as a multiple of the GPU's warp size)
    //
    
    image_dimensions_.push_back(((r_space.matrixSize().x()+warp_size-1)/warp_size)*warp_size);
    image_dimensions_.push_back(((r_space.matrixSize().y()+warp_size-1)/warp_size)*warp_size);

    image_dimensions_recon_.push_back(((static_cast<unsigned int>(std::ceil(r_space.matrixSize().x()*get_double_value(std::string("reconstruction_os_factor_x").c_str())))+warp_size-1)/warp_size)*warp_size);  
    image_dimensions_recon_.push_back(((static_cast<unsigned int>(std::ceil(r_space.matrixSize().y()*get_double_value(std::string("reconstruction_os_factor_y").c_str())))+warp_size-1)/warp_size)*warp_size);
    
    image_dimensions_recon_os_ = uintd2
      (((static_cast<unsigned int>(std::ceil(image_dimensions_recon_[0]*oversampling_factor_))+warp_size-1)/warp_size)*warp_size,
       ((static_cast<unsigned int>(std::ceil(image_dimensions_recon_[1]*oversampling_factor_))+warp_size-1)/warp_size)*warp_size);
    
    // In case the warp_size constraint kicked in
    oversampling_factor_ = float(image_dimensions_recon_os_[0])/float(image_dimensions_recon_[0]); 
    
    GADGET_DEBUG2("matrix_size_x : %d, recon: %d, recon_os: %d\n", 
		  image_dimensions_[0], image_dimensions_recon_[0], image_dimensions_recon_os_[0]);

    GADGET_DEBUG2("matrix_size_y : %d, recon: %d, recon_os: %d\n", 
		  image_dimensions_[1], image_dimensions_recon_[1], image_dimensions_recon_os_[1]);
    
    fov_.push_back(r_space.fieldOfView_mm().x());
    fov_.push_back(r_space.fieldOfView_mm().y());
    fov_.push_back(r_space.fieldOfView_mm().z());

    slices_ = e_limits.slice().present() ? e_limits.slice().get().maximum() + 1 : 1;
    sets_ = e_limits.set().present() ? e_limits.set().get().maximum() + 1 : 1;
    
    // Allocate profile queues
    // - one queue for the currently incoming frame
    // - one queue for the next reconstruction

    frame_profiles_queue_ = boost::shared_array< ACE_Message_Queue<ACE_MT_SYNCH> >(new ACE_Message_Queue<ACE_MT_SYNCH>[slices_*sets_]);
    recon_profiles_queue_ = boost::shared_array< ACE_Message_Queue<ACE_MT_SYNCH> >(new ACE_Message_Queue<ACE_MT_SYNCH>[slices_*sets_]);
    image_headers_queue_ = boost::shared_array< ACE_Message_Queue<ACE_MT_SYNCH> >(new ACE_Message_Queue<ACE_MT_SYNCH>[slices_*sets_]);

    size_t bsize = sizeof(GadgetContainerMessage< hoNDArray< std::complex<float> > >)*image_dimensions_[0]*10;

    for( unsigned int i=0; i<slices_*sets_; i++ ){
      frame_profiles_queue_[i].high_water_mark(bsize);
      frame_profiles_queue_[i].low_water_mark(bsize);
    }
    
    bsize *= (rotations_per_reconstruction_+1);
    
    for( unsigned int i=0; i<slices_*sets_; i++ ){
      recon_profiles_queue_[i].high_water_mark(bsize);
      recon_profiles_queue_[i].low_water_mark(bsize);
      }

    // Define some profile counters for book-keeping
    //

    previous_profile_ = boost::shared_array<long>(new long[slices_*sets_]);
    image_counter_ = boost::shared_array<long>(new long[slices_*sets_]);
    profiles_counter_frame_= boost::shared_array<long>(new long[slices_*sets_]);
    profiles_counter_global_= boost::shared_array<long>(new long[slices_*sets_]);
    profiles_per_frame_= boost::shared_array<long>(new long[slices_*sets_]);
    frames_per_rotation_= boost::shared_array<long>(new long[slices_*sets_]);
    buffer_frames_per_rotation_= boost::shared_array<long>(new long[slices_*sets_]);
    buffer_update_needed_ = boost::shared_array<bool>(new bool[slices_*sets_]);
    reconfigure_ = boost::shared_array<bool>(new bool[slices_*sets_]);
    num_coils_ = boost::shared_array<unsigned int>(new unsigned int[slices_*sets_]);
    
    if( !previous_profile_.get() ||
	!image_counter_.get() || 
	!profiles_counter_frame_.get() ||
	!profiles_counter_global_.get() ||
	!profiles_per_frame_.get() || 
	!frames_per_rotation_.get() ||
	!buffer_frames_per_rotation_.get() ||
	!buffer_update_needed_.get() ||
	!num_coils_.get() ||
	!reconfigure_ ){
      GADGET_DEBUG1("Failed to allocate host memory (1)\n");
      return GADGET_FAIL;
    }

    for( unsigned int i=0; i<slices_*sets_; i++ ){

      previous_profile_[i] = -1;
      image_counter_[i] = 0;
      profiles_counter_frame_[i] = 0;
      profiles_counter_global_[i] = 0;
      profiles_per_frame_[i] = get_int_value(std::string("profiles_per_frame").c_str());
      frames_per_rotation_[i] = get_int_value(std::string("frames_per_rotation").c_str());
      buffer_frames_per_rotation_[i] = get_int_value(std::string("buffer_frames_per_rotation").c_str());
      num_coils_[i] = 0;
      buffer_update_needed_[i] = true;
      reconfigure_[i] = true;

      // Assign some default values ("upper bound estimates") of the (possibly) unknown entities
      //
      
      if( profiles_per_frame_[i] == 0 ){
	profiles_per_frame_[i] = image_dimensions_[0];
      }
      
      if( frames_per_rotation_[i] == 0 ){
	if( mode_ == 2 ) // golden angle
	  frames_per_rotation_[i] = 1;
	else
	  frames_per_rotation_[i] = image_dimensions_[0]/profiles_per_frame_[i];
      }

      bsize = sizeof(GadgetContainerMessage<ISMRMRD::ImageHeader>)*100*
	std::max(1L, frames_per_rotation_[i]*rotations_per_reconstruction_);
    
      image_headers_queue_[i].high_water_mark(bsize);
      image_headers_queue_[i].low_water_mark(bsize);
    }
        
    position_ = boost::shared_array<float[3]>(new float[slices_*sets_][3]);
    read_dir_ = boost::shared_array<float[3]>(new float[slices_*sets_][3]);
    phase_dir_ = boost::shared_array<float[3]>(new float[slices_*sets_][3]);
    slice_dir_ = boost::shared_array<float[3]>(new float[slices_*sets_][3]);

    if( !position_.get() || !read_dir_.get() || !phase_dir_.get() || !slice_dir_.get() ){
      GADGET_DEBUG1("Failed to allocate host memory (2)\n");
      return GADGET_FAIL;
    }

    for( unsigned int i=0; i<slices_*sets_; i++ ){
      (position_[i])[0] = (position_[i])[1] = (position_[i])[2] = 0.0f;
      (read_dir_[i])[0] = (read_dir_[i])[1] = (read_dir_[i])[2] = 0.0f;
      (phase_dir_[i])[0] = (phase_dir_[i])[1] = (phase_dir_[i])[2] = 0.0f;
      (slice_dir_[i])[0] = (slice_dir_[i])[1] = (slice_dir_[i])[2] = 0.0f;
    }

    // Allocate accumulation buffer
    //

    if( buffer_using_solver_ )
      acc_buffer_cg_ = boost::shared_array< cuSenseBufferCg<float,2> >(new cuSenseBufferCg<float,2>[slices_*sets_]);
    else
      acc_buffer_ = boost::shared_array< cuSenseBuffer<float,2> >(new cuSenseBuffer<float,2>[slices_*sets_]);
    
    // Allocate remaining shared_arrays
    //
    
    csm_host_ = boost::shared_array< hoNDArray<float_complext> >(new hoNDArray<float_complext>[slices_*sets_]);
    reg_host_ = boost::shared_array< hoNDArray<float_complext> >(new hoNDArray<float_complext>[slices_*sets_]);

    host_traj_recon_ = boost::shared_array< hoNDArray<floatd2> >(new hoNDArray<floatd2>[slices_*sets_]);
    host_weights_recon_ = boost::shared_array< hoNDArray<float> >(new hoNDArray<float>[slices_*sets_]);

    if( !csm_host_.get() || !reg_host_.get() || !host_traj_recon_.get() || !host_weights_recon_ ){
      GADGET_DEBUG1("Failed to allocate host memory (3)\n");
      return GADGET_FAIL;
    }

    return GADGET_OK;
  }

  int gpuRadialSensePrepGadget::
  process(GadgetContainerMessage<ISMRMRD::AcquisitionHeader> *m1,
	  GadgetContainerMessage< hoNDArray< std::complex<float> > > *m2)
  {
    // Noise should have been consumed by the noise adjust (if in the gadget chain)
    //
    
    bool is_noise = ISMRMRD::FlagBit(ISMRMRD::ACQ_IS_NOISE_MEASUREMENT).isSet(m1->getObjectPtr()->flags);
    if (is_noise) { 
      m1->release();
      return GADGET_OK;
    }

    unsigned int profile = m1->getObjectPtr()->idx.kspace_encode_step_1;
    unsigned int slice = m1->getObjectPtr()->idx.slice;
    unsigned int set = m1->getObjectPtr()->idx.set;

    // Get a pointer to the accumulation buffer. 
    //

    cuSenseBuffer<float,2> *acc_buffer = (buffer_using_solver_) ? &acc_buffer_cg_[set*slices_+slice] : &acc_buffer_[set*slices_+slice];

    //GADGET_DEBUG1("gpuRadialSensePrepGadget::process\n");

    boost::shared_ptr<GPUTimer> process_timer;
    if( output_timing_ )
      process_timer = boost::shared_ptr<GPUTimer>( new GPUTimer("gpuRadialSensePrepGadget::process()") );

    // Have the imaging plane changed?
    //

    if( !vec_equal(position_[set*slices_+slice], m1->getObjectPtr()->position) ||
	!vec_equal(read_dir_[set*slices_+slice], m1->getObjectPtr()->read_dir) || 
	!vec_equal(phase_dir_[set*slices_+slice], m1->getObjectPtr()->phase_dir) ||
	!vec_equal(slice_dir_[set*slices_+slice], m1->getObjectPtr()->slice_dir) ){
      
      // Yes indeed, clear the accumulation buffer
      acc_buffer->clear();
      buffer_update_needed_[set*slices_+slice] = true;
      
      memcpy(position_[set*slices_+slice],m1->getObjectPtr()->position,3*sizeof(float));
      memcpy(read_dir_[set*slices_+slice],m1->getObjectPtr()->read_dir,3*sizeof(float));
      memcpy(phase_dir_[set*slices_+slice],m1->getObjectPtr()->phase_dir,3*sizeof(float));
      memcpy(slice_dir_[set*slices_+slice],m1->getObjectPtr()->slice_dir,3*sizeof(float));
    }
    
    // Only when the first profile arrives, do we know the #samples/profile
    //

    if( samples_per_profile_ == -1 )      
      samples_per_profile_ = m1->getObjectPtr()->number_of_samples;
    
    if( samples_per_profile_ != m1->getObjectPtr()->number_of_samples ){
      GADGET_DEBUG1("Unexpected change in the incoming profiles' lengths\n");
      return GADGET_FAIL;
    }
    
    bool new_frame_detected = false;

    // Reconfigure at first pass
    // - or if the number of coil changes
    // - or if the reconfigure_ flag is set

    if( num_coils_[set*slices_+slice] != m1->getObjectPtr()->active_channels ){
      GADGET_DEBUG1("Reconfiguring due to change in the number of coils\n");
      num_coils_[set*slices_+slice] = m1->getObjectPtr()->active_channels;
      reconfigure(set, slice);
    }

    if( reconfigure_[set*slices_+slice] ){
      GADGET_DEBUG1("Reconfiguring due to boolean indicator\n");
      reconfigure(set, slice);
    }

    // Keep track of the incoming profile ids (mode dependent)
    // - to determine the number of profiles per frame
    // - to determine the number of frames per rotation
    //

    if (previous_profile_[set*slices_+slice] >= 0) {

      if ( profile > previous_profile_[set*slices_+slice]) { // this is not the last profile in the frame
	if( mode_ == 0 && get_int_value(std::string("frames_per_rotation").c_str()) == 0 ){
	  unsigned int acceleration_factor = profile - previous_profile_[set*slices_+slice];
	  if( acceleration_factor != frames_per_rotation_[set*slices_+slice] ){
	    GADGET_DEBUG1("Reconfiguring due to change in acceleration factor\n");
	    frames_per_rotation_[set*slices_+slice] = acceleration_factor;
	    reconfigure(set, slice);
	  }
	}
      }
      else{ // This is the first profile in a new frame
	if( get_int_value(std::string("profiles_per_frame").c_str()) == 0 && // make sure the user did not specify a desired value for this variable
	    profiles_counter_frame_[set*slices_+slice] > 0 &&
	    profiles_counter_frame_[set*slices_+slice] != profiles_per_frame_[set*slices_+slice] ){ // a new acceleration factor is detected
	  GADGET_DEBUG1("Reconfiguring due to new slice detection\n");
	  new_frame_detected = true;
	  profiles_per_frame_[set*slices_+slice] = profiles_counter_frame_[set*slices_+slice];
	  if( mode_ == 1 && get_int_value(std::string("frames_per_rotation").c_str()) == 0 )
	    frames_per_rotation_[set*slices_+slice] = image_dimensions_[0]/profiles_per_frame_[set*slices_+slice];
	  reconfigure(set, slice);
	}
      }
    }
    previous_profile_[set*slices_+slice] = profile;

    // Enqueue profile
    // - if 'new_frame_detected' the current profile does not belong to the current frame and we delay enqueing

    if( !new_frame_detected ) {
      
      // Memory handling is easier if we make copies for our internal queues
      frame_profiles_queue_[set*slices_+slice].enqueue_tail(duplicate_profile(m2));
      recon_profiles_queue_[set*slices_+slice].enqueue_tail(duplicate_profile(m2));
    }

    // If the profile is the last of a "true frame" (ignoring any sliding window profiles)
    // - then update the accumulation buffer

    bool is_last_profile_in_frame = (profiles_counter_frame_[set*slices_+slice] == profiles_per_frame_[set*slices_+slice]-1);
    is_last_profile_in_frame |= new_frame_detected;

    if( is_last_profile_in_frame ){

      // Extract this frame's samples to update the csm/regularization buffer
      //

      boost::shared_ptr< hoNDArray<float_complext> > host_samples = 
	extract_samples_from_queue( &frame_profiles_queue_[set*slices_+slice], false, set, slice );

      if( host_samples.get() == 0x0 ){
	GADGET_DEBUG1("Failed to extract frame data from queue\n");
	return GADGET_FAIL;
      }
      
      cuNDArray<float_complext> samples( host_samples.get() );
      
      long profile_offset = profiles_counter_global_[set*slices_+slice] - ((new_frame_detected) ? 1 : 0);
      boost::shared_ptr< cuNDArray<floatd2> > traj = calculate_trajectory_for_frame(profile_offset, set, slice);

      buffer_update_needed_[set*slices_+slice] |= acc_buffer->add_frame_data( &samples, traj.get() );
    }
    
    // Are we ready to reconstruct (downstream)?
    //
    
    long profiles_per_reconstruction = profiles_per_frame_[set*slices_+slice];
    
    if( rotations_per_reconstruction_ > 0 )
      profiles_per_reconstruction *= (frames_per_rotation_[set*slices_+slice]*rotations_per_reconstruction_);
    
    bool is_last_profile_in_reconstruction = ( recon_profiles_queue_[set*slices_+slice].message_count() == profiles_per_reconstruction );
        
    // Prepare the image header for this frame
    // - if this is indeed the last profile of a new frame
    // - or if we are about to reconstruct due to 'sliding_window_profiles_' > 0

    if( is_last_profile_in_frame || 
	(is_last_profile_in_reconstruction && image_headers_queue_[set*slices_+slice].message_count() == 0) ){
      
      GadgetContainerMessage<ISMRMRD::ImageHeader> *header = new GadgetContainerMessage<ISMRMRD::ImageHeader>();
      ISMRMRD::AcquisitionHeader *base_head = m1->getObjectPtr();

      {
	// Initialize header to all zeroes (there is a few fields we do not set yet)
	ISMRMRD::ImageHeader tmp = {0};
	*(header->getObjectPtr()) = tmp;
      }

      header->getObjectPtr()->version = base_head->version;

      header->getObjectPtr()->matrix_size[0] = image_dimensions_recon_[0];
      header->getObjectPtr()->matrix_size[1] = image_dimensions_recon_[1];
      header->getObjectPtr()->matrix_size[2] = std::max(1L,frames_per_rotation_[set*slices_+slice]*rotations_per_reconstruction_);

      header->getObjectPtr()->field_of_view[0] = fov_[0];
      header->getObjectPtr()->field_of_view[1] = fov_[1];
      header->getObjectPtr()->field_of_view[2] = fov_[2];

      header->getObjectPtr()->channels = num_coils_[set*slices_+slice];
      header->getObjectPtr()->slice = base_head->idx.slice;
      header->getObjectPtr()->set = base_head->idx.set;

      header->getObjectPtr()->acquisition_time_stamp = base_head->acquisition_time_stamp;
      memcpy(header->getObjectPtr()->physiology_time_stamp, base_head->physiology_time_stamp, sizeof(uint32_t)*ISMRMRD_PHYS_STAMPS);

      memcpy(header->getObjectPtr()->position, base_head->position, sizeof(float)*3);
      memcpy(header->getObjectPtr()->read_dir, base_head->read_dir, sizeof(float)*3);
      memcpy(header->getObjectPtr()->phase_dir, base_head->phase_dir, sizeof(float)*3);
      memcpy(header->getObjectPtr()->slice_dir, base_head->slice_dir, sizeof(float)*3);
      memcpy(header->getObjectPtr()->patient_table_position, base_head->patient_table_position, sizeof(float)*3);

      header->getObjectPtr()->image_data_type = ISMRMRD::DATA_COMPLEX_FLOAT;
      header->getObjectPtr()->image_index = image_counter_[set*slices_+slice]++; 
      header->getObjectPtr()->image_series_index = set*slices_+slice;

      image_headers_queue_[set*slices_+slice].enqueue_tail(header);
    }
    
    // If it is time to reconstruct (downstream) then prepare the Sense job
    // 

    if( is_last_profile_in_reconstruction ){
      
      // Update csm and regularization images if the buffer has changed (completed a cycle) 
      // - and at the first pass
      
      if( buffer_update_needed_[set*slices_+slice] || 
	  csm_host_[set*slices_+slice].get_number_of_elements() == 0 || 
	  reg_host_[set*slices_+slice].get_number_of_elements() == 0 ){

	// Get the accumulated coil images
	//

	boost::shared_ptr< cuNDArray<float_complext> > csm_data = acc_buffer->get_accumulated_coil_images();

	if( !csm_data.get() ){
	  GADGET_DEBUG1("Error during accumulation buffer computation\n");
	  return GADGET_FAIL;
	}            
	
	// Estimate CSM
	//

	boost::shared_ptr< cuNDArray<float_complext> > csm = estimate_b1_map<float,2>( csm_data.get() );

	if( !csm.get() ){
	  GADGET_DEBUG1("Error during coil estimation\n");
	  return GADGET_FAIL;
	}            

	acc_buffer->set_csm(csm);
	csm_host_[set*slices_+slice] = *(csm->to_host());
	
	// Compute regularization image
	//

	boost::shared_ptr< cuNDArray<float_complext> > reg_image;
	
	if( buffer_using_solver_ && mode_ == 2 ){
	  ((cuSenseBufferCg<float,2>*)acc_buffer)->preprocess
	    ( calculate_trajectory_for_rhs( profiles_counter_global_[set*slices_+slice] - ((new_frame_detected) ? 1 : 0), set, slice).get());
	}

	reg_image = acc_buffer->get_combined_coil_image();
	
	if( !reg_image.get() ){
	  GADGET_DEBUG1("Error computing regularization image\n");
	  return GADGET_FAIL;
	}            
	
	reg_host_[set*slices_+slice] = *(reg_image->to_host());
		
	/*
	static int counter = 0;
	char filename[256];
	sprintf((char*)filename, "reg_%d.cplx", counter);
	write_nd_array<float_complext>( reg_host_[set*slices_+slice].get(), filename );
	counter++; */

	buffer_update_needed_[set*slices_+slice] = false;
      }

      // Prepare data array of the profiles for the downstream reconstruction
      //
      
      boost::shared_ptr< hoNDArray<float_complext> > samples_host = 
	extract_samples_from_queue( &recon_profiles_queue_[set*slices_+slice], true, set, slice );
      
      if( samples_host.get() == 0x0 ){
	GADGET_DEBUG1("Failed to extract frame data from queue\n");
	return GADGET_FAIL;
      }
           
      // The trajectory needs to be updated on the fly:
      // - for golden ratio based acquisitions
      // - when we are reconstructing frame-by-frame
      
      if( mode_ == 2 || rotations_per_reconstruction_ == 0 ){
	calculate_trajectory_for_reconstruction
	  ( profiles_counter_global_[set*slices_+slice] - ((new_frame_detected) ? 1 : 0), set, slice );
      }
      
      // Set up Sense job
      //

      GadgetContainerMessage< SenseJob >* m4 = new GadgetContainerMessage< SenseJob >();
	
      m4->getObjectPtr()->dat_host_ = samples_host;
      m4->getObjectPtr()->tra_host_ = boost::shared_ptr< hoNDArray<floatd2> >(new hoNDArray<floatd2>(host_traj_recon_[set*slices_+slice]));
      m4->getObjectPtr()->dcw_host_ = boost::shared_ptr< hoNDArray<float> >(new hoNDArray<float>(host_weights_recon_[set*slices_+slice]));
      m4->getObjectPtr()->csm_host_ = boost::shared_ptr< hoNDArray<float_complext> >( new hoNDArray<float_complext>(csm_host_[set*slices_+slice]));
      m4->getObjectPtr()->reg_host_ = boost::shared_ptr< hoNDArray<float_complext> >( new hoNDArray<float_complext>(reg_host_[set*slices_+slice]));

      // Pull the image headers out of the queue
      //

      long frames_per_reconstruction = 
	std::max( 1L, frames_per_rotation_[set*slices_+slice]*rotations_per_reconstruction_ );
      
      if( image_headers_queue_[set*slices_+slice].message_count() != frames_per_reconstruction ){
	m4->release();
	GADGET_DEBUG2("Unexpected size of image header queue: %d, %d\n", 
		      image_headers_queue_[set*slices_+slice].message_count(), frames_per_reconstruction);
	return GADGET_FAIL;
      }

      m4->getObjectPtr()->image_headers_ =
	boost::shared_array<ISMRMRD::ImageHeader>( new ISMRMRD::ImageHeader[frames_per_reconstruction] );
      
      for( unsigned int i=0; i<frames_per_reconstruction; i++ ){	

	ACE_Message_Block *mbq;

	if( image_headers_queue_[set*slices_+slice].dequeue_head(mbq) < 0 ) {
	  m4->release();
	  GADGET_DEBUG1("Image header dequeue failed\n");
	  return GADGET_FAIL;
	}
	
	GadgetContainerMessage<ISMRMRD::ImageHeader> *m = AsContainerMessage<ISMRMRD::ImageHeader>(mbq);
	m4->getObjectPtr()->image_headers_[i] = *m->getObjectPtr();

	// In sliding window mode the header might need to go back at the end of the queue for reuse
	// 
	
	if( i >= frames_per_reconstruction-sliding_window_rotations_*frames_per_rotation_[set*slices_+slice] ){
	  image_headers_queue_[set*slices_+slice].enqueue_tail(m);
	}
	else {
	  m->release();
	}
      }      
      
      // The Sense Job needs an image header as well. 
      // Let us just copy the initial one...

      GadgetContainerMessage<ISMRMRD::ImageHeader> *m3 = new GadgetContainerMessage<ISMRMRD::ImageHeader>;
      *m3->getObjectPtr() = m4->getObjectPtr()->image_headers_[0];
      m3->cont(m4);
      
      //GADGET_DEBUG1("Putting job on queue\n");
      
      if (this->next()->putq(m3) < 0) {
	GADGET_DEBUG1("Failed to put job on queue.\n");
	m3->release();
	return GADGET_FAIL;
      }
    }
    
    if( is_last_profile_in_frame )
      profiles_counter_frame_[set*slices_+slice] = 0;
    else{
      profiles_counter_frame_[set*slices_+slice]++;
    }

    if( new_frame_detected ){

      // This is the first profile of the next frame, enqueue.
      // We have encountered deadlocks if the same profile is enqueued twice in different queues. Hence the copy.
      
      frame_profiles_queue_[set*slices_+slice].enqueue_tail(duplicate_profile(m2));
      recon_profiles_queue_[set*slices_+slice].enqueue_tail(duplicate_profile(m2)); 

      profiles_counter_frame_[set*slices_+slice]++;
    }

    profiles_counter_global_[set*slices_+slice]++;

    if( output_timing_ )
      process_timer.reset();
    
    m1->release(); // the internal queues hold copies
    return GADGET_OK;
  }
  
  int 
  gpuRadialSensePrepGadget::calculate_trajectory_for_reconstruction(long profile_offset, unsigned int set, unsigned int slice)
  {   
    //GADGET_DEBUG1("Calculating trajectory for reconstruction\n");

    switch(mode_){
      
    case 0:
    case 1:
      {
	if( rotations_per_reconstruction_ == 0 ){

	  long local_frame = (profile_offset/profiles_per_frame_[set*slices_+slice])%frames_per_rotation_[set*slices_+slice];
	  float angular_offset = M_PI/float(profiles_per_frame_[set*slices_+slice])*float(local_frame)/float(frames_per_rotation_[set*slices_+slice]);	  

	  host_traj_recon_[set*slices_+slice] = *compute_radial_trajectory_fixed_angle_2d<float>
	    ( samples_per_profile_, profiles_per_frame_[set*slices_+slice], 1, angular_offset )->to_host();	
	}
	else{
	  host_traj_recon_[set*slices_+slice] = *compute_radial_trajectory_fixed_angle_2d<float>
	    ( samples_per_profile_, profiles_per_frame_[set*slices_+slice], frames_per_rotation_[set*slices_+slice] )->to_host();
	}
      }
      break;
      
    case 2:
      {
	if( rotations_per_reconstruction_ == 0 ){	  
	  unsigned int first_profile_in_reconstruction = std::max(0L, profile_offset-profiles_per_frame_[set*slices_+slice]+1);
	  host_traj_recon_[set*slices_+slice] = *compute_radial_trajectory_golden_ratio_2d<float>
	    ( samples_per_profile_, profiles_per_frame_[set*slices_+slice], 1, first_profile_in_reconstruction )->to_host();	
	}
	else{
	  unsigned int first_profile_in_reconstruction = 
	    std::max(0L, profile_offset-profiles_per_frame_[set*slices_+slice]*frames_per_rotation_[set*slices_+slice]*rotations_per_reconstruction_+1);
	  host_traj_recon_[set*slices_+slice] = *compute_radial_trajectory_golden_ratio_2d<float>
	    ( samples_per_profile_, profiles_per_frame_[set*slices_+slice], frames_per_rotation_[set*slices_+slice]*rotations_per_reconstruction_, first_profile_in_reconstruction )->to_host();
	}	  
      }
      break;
	
    default:
      GADGET_DEBUG1("Illegal trajectory mode\n");
      return GADGET_FAIL;
      break;
    }
    return GADGET_OK;
  }  

  int
  gpuRadialSensePrepGadget::calculate_density_compensation_for_reconstruction( unsigned int set, unsigned int slice)
  {
    //GADGET_DEBUG1("Calculating dcw for reconstruction\n");
    
    switch(mode_){
      
    case 0:
    case 1:
      host_weights_recon_[set*slices_+slice] = *compute_radial_dcw_fixed_angle_2d<float>
	( samples_per_profile_, profiles_per_frame_[set*slices_+slice], oversampling_factor_, 
	  1.0f/(float(samples_per_profile_)/float(image_dimensions_recon_[0])) )->to_host();
      break;
      
    case 2:
      host_weights_recon_[set*slices_+slice] = *compute_radial_dcw_golden_ratio_2d<float>
	( samples_per_profile_, profiles_per_frame_[set*slices_+slice], oversampling_factor_, 
	  1.0f/(float(samples_per_profile_)/float(image_dimensions_recon_[0])) )->to_host();
      break;
      
    default:
      GADGET_DEBUG1("Illegal dcw mode\n");
      return GADGET_FAIL;
      break;
    }
    return GADGET_OK;
  }
  
  boost::shared_ptr< cuNDArray<floatd2> > 
  gpuRadialSensePrepGadget::calculate_trajectory_for_frame(long profile_offset, unsigned int set, unsigned int slice)
  {
    //GADGET_DEBUG1("Calculating trajectory for buffer frame\n");

    boost::shared_ptr< cuNDArray<floatd2> > result;

    switch(mode_){

    case 0:
    case 1:
      {
	long local_frame = (profile_offset/profiles_per_frame_[set*slices_+slice])%frames_per_rotation_[set*slices_+slice];
	float angular_offset = M_PI/float(profiles_per_frame_[set*slices_+slice])*float(local_frame)/float(frames_per_rotation_[set*slices_+slice]);	  

	result = compute_radial_trajectory_fixed_angle_2d<float>
	  ( samples_per_profile_, profiles_per_frame_[set*slices_+slice], 1, angular_offset );  
      }
      break;
	
    case 2:
      { 
	unsigned int first_profile_in_buffer = std::max(0L, profile_offset-profiles_per_frame_[set*slices_+slice]+1);

	result  = compute_radial_trajectory_golden_ratio_2d<float>
	  ( samples_per_profile_, profiles_per_frame_[set*slices_+slice], 1, first_profile_in_buffer );
      }
      break;	
	
    default:
      GADGET_DEBUG1("Illegal trajectory mode\n");
      break;
    }
    
    return result;
  }

  boost::shared_ptr< cuNDArray<float> >
  gpuRadialSensePrepGadget::calculate_density_compensation_for_frame(unsigned int set, unsigned int slice)
  {    
    //GADGET_DEBUG1("Calculating dcw for buffer frame\n");

    switch(mode_){
      
    case 0:
    case 1:
      return compute_radial_dcw_fixed_angle_2d<float>
	( samples_per_profile_, profiles_per_frame_[set*slices_+slice], oversampling_factor_, 1.0f/(float(samples_per_profile_)/float(image_dimensions_recon_[0])) );
      break;
      
    case 2:
      return compute_radial_dcw_golden_ratio_2d<float>
	( samples_per_profile_, profiles_per_frame_[set*slices_+slice], oversampling_factor_, 1.0f/(float(samples_per_profile_)/float(image_dimensions_recon_[0])) );
      break;
      
    default:
      GADGET_DEBUG1("Illegal dcw mode\n");
      return boost::shared_ptr< cuNDArray<float> >();
      break;
    }   
  }


  boost::shared_ptr< cuNDArray<floatd2> > 
  gpuRadialSensePrepGadget::calculate_trajectory_for_rhs(long profile_offset, unsigned int set, unsigned int slice)
  {
    //GADGET_DEBUG1("Calculating trajectory for rhs\n");

    switch(mode_){

    case 0:
    case 1:
      return compute_radial_trajectory_fixed_angle_2d<float>
	( samples_per_profile_, profiles_per_frame_[set*slices_+slice]*buffer_frames_per_rotation_[set*slices_+slice], 1 );
      break;
	
    case 2:
      { 
	unsigned int first_profile = 
	  std::max(0L, profile_offset-profiles_per_frame_[set*slices_+slice]*
		   buffer_frames_per_rotation_[set*slices_+slice]*
		   buffer_length_in_rotations_+1);

	return compute_radial_trajectory_golden_ratio_2d<float>
	  ( samples_per_profile_, 
	    profiles_per_frame_[set*slices_+slice]*
	    buffer_frames_per_rotation_[set*slices_+slice]*buffer_length_in_rotations_, 
	    1, first_profile );
      }
      break;	
	
    default:
      GADGET_DEBUG1("Illegal trajectory mode\n");
      return boost::shared_ptr< cuNDArray<floatd2> >();
      break;
    }
  }
  
  boost::shared_ptr< cuNDArray<float> >
  gpuRadialSensePrepGadget::calculate_density_compensation_for_rhs(unsigned int set, unsigned int slice)
  {
    //GADGET_DEBUG1("Calculating dcw for rhs\n");
    
    switch(mode_){
      
    case 0:
    case 1:
      {
	unsigned int num_profiles = 
	  profiles_per_frame_[set*slices_+slice]*buffer_frames_per_rotation_[set*slices_+slice];

	return compute_radial_dcw_fixed_angle_2d<float>
	  ( samples_per_profile_, num_profiles, oversampling_factor_, 
	    1.0f/(float(samples_per_profile_)/float(image_dimensions_recon_[0])) );
      }
      break;
      
    case 2:
      {
	unsigned int num_profiles = 
	  profiles_per_frame_[set*slices_+slice]*buffer_frames_per_rotation_[set*slices_+slice]*buffer_length_in_rotations_;

	return compute_radial_dcw_golden_ratio_2d<float>
	  ( samples_per_profile_, num_profiles, oversampling_factor_, 
	    1.0f/(float(samples_per_profile_)/float(image_dimensions_recon_[0])) );
      }
      break;
      
    default:
      GADGET_DEBUG1("Illegal dcw mode\n");
      return boost::shared_ptr< cuNDArray<float> >();
      break;
    }
  }

  boost::shared_ptr< hoNDArray<float_complext> > gpuRadialSensePrepGadget::
  extract_samples_from_queue( ACE_Message_Queue<ACE_MT_SYNCH> *queue, bool sliding_window,
			      unsigned int set, unsigned int slice )
  {    
    //GADGET_DEBUG1("Emptying queue...\n");

    unsigned int profiles_buffered = queue->message_count();
    
    std::vector<unsigned int> dims;
    dims.push_back(samples_per_profile_*profiles_buffered);
    dims.push_back(num_coils_[set*slices_+slice]);
    
    boost::shared_ptr< hoNDArray<float_complext> > host_samples(new hoNDArray<float_complext>(&dims));
    
    for (unsigned int p=0; p<profiles_buffered; p++) {

      ACE_Message_Block* mbq;
      if (queue->dequeue_head(mbq) < 0) {
	GADGET_DEBUG1("Message dequeue failed\n");
	return boost::shared_ptr< hoNDArray<float_complext> >();
      }
      
      GadgetContainerMessage< hoNDArray< std::complex<float> > > *daq = AsContainerMessage<hoNDArray< std::complex<float> > >(mbq);
	
      if (!daq) {
	GADGET_DEBUG1("Unable to interpret data on message queue\n");
	return boost::shared_ptr< hoNDArray<float_complext> >();
      }
	
      for (unsigned int c = 0; c < num_coils_[set*slices_+slice]; c++) {
	
	float_complext *data_ptr = host_samples->get_data_ptr();
	data_ptr += c*samples_per_profile_*profiles_buffered+p*samples_per_profile_;
	    
	std::complex<float> *r_ptr = daq->getObjectPtr()->get_data_ptr();
	r_ptr += c*daq->getObjectPtr()->get_size(0);
	  
	memcpy(data_ptr,r_ptr,samples_per_profile_*sizeof(float_complext));
      }

      // In sliding window mode the profile might need to go back at the end of the queue
      // 
      
      long profiles_in_sliding_window = sliding_window_profiles_ + 
	profiles_per_frame_[set*slices_+slice]*frames_per_rotation_[set*slices_+slice]*sliding_window_rotations_;

      if( sliding_window && p >= (profiles_buffered-profiles_in_sliding_window) )
	queue->enqueue_tail(mbq);
      else
	mbq->release();
    } 
    
    return host_samples;
  }
  
  GadgetContainerMessage< hoNDArray< std::complex<float> > >*
  gpuRadialSensePrepGadget::duplicate_profile( GadgetContainerMessage< hoNDArray< std::complex<float> > > *profile )
  {
    GadgetContainerMessage< hoNDArray< std::complex<float> > > *copy = 
      new GadgetContainerMessage< hoNDArray< std::complex<float> > >();
    
    *copy->getObjectPtr() = *profile->getObjectPtr();
    
    return copy;
  }

  void gpuRadialSensePrepGadget::reconfigure(unsigned int set, unsigned int slice)
  {    
    GADGET_DEBUG2("\nReconfiguring:\n#profiles/frame:%d\n#frames/rotation: %d\n#rotations/reconstruction:%d\n", 
		  profiles_per_frame_[set*slices_+slice], frames_per_rotation_[set*slices_+slice], rotations_per_reconstruction_);

    calculate_trajectory_for_reconstruction(0, set, slice);
    calculate_density_compensation_for_reconstruction(set, slice);
    
    buffer_frames_per_rotation_[set*slices_+slice] = get_int_value(std::string("buffer_frames_per_rotation").c_str());

    if( buffer_frames_per_rotation_[set*slices_+slice] == 0 ){
      if( mode_ == 2 )
	buffer_frames_per_rotation_[set*slices_+slice] = 
	  image_dimensions_recon_os_[0]/profiles_per_frame_[set*slices_+slice];
      else
	buffer_frames_per_rotation_[set*slices_+slice] = frames_per_rotation_[set*slices_+slice];
    }
    
    cuSenseBuffer<float,2> *acc_buffer = (buffer_using_solver_) ? &acc_buffer_cg_[set*slices_+slice] : &acc_buffer_[set*slices_+slice];

    acc_buffer->setup( from_std_vector<unsigned int,2>(image_dimensions_recon_), image_dimensions_recon_os_, 
		       kernel_width_, num_coils_[set*slices_+slice], 
		       buffer_length_in_rotations_, buffer_frames_per_rotation_[set*slices_+slice] );
    
    boost::shared_ptr< cuNDArray<float> > device_weights_frame = calculate_density_compensation_for_frame(set, slice);
    acc_buffer->set_dcw(device_weights_frame);

    if( buffer_using_solver_ ){
      ((cuSenseBufferCg<float,2>*) acc_buffer)->set_dcw_for_rhs(calculate_density_compensation_for_rhs(set, slice));
      ((cuSenseBufferCg<float,2>*) acc_buffer)->preprocess(calculate_trajectory_for_rhs(0, set, slice).get());
    }
    
    reconfigure_[set*slices_+slice] = false;
  }

  GADGET_FACTORY_DECLARE(gpuRadialSensePrepGadget)
}