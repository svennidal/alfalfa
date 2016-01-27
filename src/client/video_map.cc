#include <algorithm>
#include <limits>

#include "video_map.hh"

using namespace std;
using namespace grpc;

unsigned int find_max( vector<size_t> track_ids )
{
  if ( track_ids.empty() ) {
    throw runtime_error( "video has no tracks" );
  }
  
  sort( track_ids.begin(), track_ids.end() );

  for ( unsigned int i = 0; i < track_ids.size(); i++ ) {
    if ( i != track_ids[ i ] ) {
      throw runtime_error( "video does not have contiguous track_ids starting with 0" );
    }
  }

  return track_ids.back();
}

vector<size_t> get_track_lengths( const AlfalfaVideoClient & video )
{
  unsigned int max_track_id = find_max( video.get_track_ids() );
  vector<size_t> ret;
  
  for ( unsigned int i = 0; i <= max_track_id; i++ ) {
    ret.push_back( video.get_track_size( i ) );
  }

  return ret;
}

VideoMap::VideoMap( const string & server_address, const unsigned int raster_count )
  : video_( server_address ),
    track_lengths_ ( get_track_lengths( video_ ) ),
    tracks_(),
    shown_frame_counts_(),
    total_shown_frame_count_( raster_count )
{
  for ( unsigned int i = 0; i < track_lengths_.size(); i++ ) {
    tracks_.emplace_back();
    shown_frame_counts_.emplace_back();
    fetchers_.emplace_back( [&] ( unsigned int track_id ) { fetch_track( track_id ); }, i );
    fetchers_.back().detach();
  }
}

void VideoMap::fetch_track( unsigned int track_id )
{
  /* start fetching the track details */
  unique_lock<mutex> lock { mutex_ };
  grpc::ClientContext client_context;
  const unsigned int tracklen = track_lengths_.at( track_id );
  auto track_infos = video_.get_abridged_frames( client_context, track_id, 0, tracklen );

  lock.unlock();
  
  AlfalfaProtobufs::AbridgedFrameInfo frame;
  while ( track_infos->Read( &frame ) ) {
    lock.lock();
    const uint64_t cumulative_length = frame.length() + (tracks_.at( track_id ).empty()
							 ? 0
							 : tracks_.at( track_id ).back().cumulative_length);
    tracks_.at( track_id ).emplace_back( frame,
					 shown_frame_counts_.at( track_id ),
					 track_id,
					 tracks_.at( track_id ).size(),
					 cumulative_length );
    if ( frame.key() ) {
      const auto & new_frame = tracks_.at( track_id ).back();
      keyframe_switches_.emplace( new_frame.timestamp,
				  make_pair( new_frame.track_id, new_frame.track_index ) );
    }

    if ( frame.shown() ) {
      shown_frame_counts_.at( track_id )++;
    }

    lock.unlock();
  }

  cerr << "all done with track " << track_id << endl;
  
  /* confirm all finished okay */
  lock.lock();
  RPC( "ClientReader::Finish", track_infos->Finish() );
  if ( tracks_.at( track_id ).size() != tracklen ) {
    throw runtime_error( "on track " + to_string( track_id ) + ", got "
			 + to_string( tracks_.at( track_id ).size() )
			 + " frames, expected " + to_string( tracklen ) );
  }
}

unsigned int VideoMap::track_length_full( const unsigned int track_id ) const
{
  return track_lengths_.at( track_id );
}

unsigned int VideoMap::track_length_now( const unsigned int track_id ) const
{
  unique_lock<mutex> lock { mutex_ };
  return tracks_.at( track_id ).size();
}

deque<AnnotatedFrameInfo> VideoMap::best_plan( unsigned int track_id,
					       unsigned int track_index,
					       const bool playing ) const
{
  deque<AnnotatedFrameInfo> ret;
  unique_lock<mutex> lock { mutex_ };

  double time_margin_available = playing ? 0 : 1.0;

  //  cerr << "best_plan( " << track_id << ", " << track_index << " )\n";
  
  while ( track_index < tracks_.at( track_id ).size() ) {
    const AnnotatedFrameInfo & start_location = tracks_.at( track_id ).at( track_index );

    vector<pair<unsigned int, unsigned int>> eligible_next_frames;
    eligible_next_frames.emplace_back( track_id, track_index );

    /* are there available keyframe switches? */
    auto range = keyframe_switches_.equal_range( start_location.timestamp );
    for ( auto sw = range.first; sw != range.second; sw++ ) {
      eligible_next_frames.push_back( sw->second );
    }

    /* if there are any feasible paths, choose purely based on quality.
       but if no paths are feasible, choose the soonest to be feasible */

    const bool exist_feasible_paths = any_of( eligible_next_frames.begin(), eligible_next_frames.end(),
					      [&] ( const pair<unsigned int, unsigned int> & x ) {
						const auto & frame = tracks_.at( x.first ).at( x.second );
						return frame.time_margin_required < time_margin_available;
					      } );

    const auto evaluator = [&] ( const pair<unsigned int, unsigned int> & x ) {
      const auto & frame = tracks_.at( x.first ).at( x.second );
      if ( exist_feasible_paths ) {
	if ( frame.time_margin_required < time_margin_available ) {
	  return -(frame.average_quality_to_end - frame.stddev_quality_to_end);
	} else {
	  return numeric_limits<double>::max();
	}
      } else {
	return frame.time_margin_required;
      }
    };

    const auto sorter = [&] ( const pair<unsigned int, unsigned int> & x,
			      const pair<unsigned int, unsigned int> & y ) {
      return evaluator( x ) < evaluator( y );
    };

    /*
    cerr << "pre sort: ";
    for ( const auto & x : eligible_next_frames ) {
      cerr << "( " << x.first << ", " << x.second << ") => " << tracks_.at( x.first ).at( x.second ).time_margin_required << "\n";
    }
    */
    sort( eligible_next_frames.begin(), eligible_next_frames.end(), sorter );
    /*
    cerr << "post sort: ";
    for ( const auto & x : eligible_next_frames ) {
      cerr << "( " << x.first << ", " << x.second << ") => " << time_margin_available - tracks_.at( x.first ).at( x.second ).time_margin_required << "\n";
    }
    */
    
    tie( track_id, track_index ) = eligible_next_frames.front();
    ret.push_back( tracks_.at( track_id ).at( track_index ) );

    time_margin_available -= tracks_.at( track_id ).at( track_index ).time_to_fetch;
    if ( ret.back().shown ) {
      time_margin_available += 1.0 / 24.0;
    }

    track_index++;
  }

  /*
  cerr << "proposing a sequence of " << ret.size() << " frames\n";
  cerr << "first frame: " << ret.front().track_id << "\n";
  cerr << "time margin for first: " << ret.front().time_margin_required << "\n";
  */
  
  return ret;
}

AnnotatedFrameInfo::AnnotatedFrameInfo( const AlfalfaProtobufs::AbridgedFrameInfo & fi,
					const unsigned int timestamp,
					const unsigned int track_id,
					const unsigned int track_index,
					const uint64_t cumulative_length )
  : offset( fi.offset() ),
    length( fi.length() ),
    frame_id( fi.frame_id() ),
    key( fi.key() ),
    shown( fi.shown() ),
    quality( fi.quality() ),
    timestamp( timestamp ),
    track_id( track_id ),
    track_index( track_index ),
    cumulative_length( cumulative_length )
{}

AnnotatedFrameInfo::AnnotatedFrameInfo( const FrameInfo & fi )
  : offset( fi.offset() ),
    length( fi.length() ),
    frame_id( fi.frame_id() ),
    key( fi.is_keyframe() ),
    shown( fi.shown() ),
    quality(),
    timestamp(),
    track_id(),
    track_index(),
    cumulative_length()
{}

void VideoMap::update_annotations( const double estimated_bytes_per_second_,
				   const unordered_map<uint64_t, pair<uint64_t, size_t>> frame_store_ )
{
  thread newthread( [&] ( const double estimated_bytes_per_second, const unordered_map<uint64_t, pair<uint64_t, size_t>> frame_store ) { 
      std::unique_lock<mutex> locked { annotation_mutex_, try_to_lock };
      if ( not locked ) {
	cerr << "skipping redundant run of frame annotations\n";
	return;
      }

      unique_lock<mutex> lock { mutex_ };
    
      for ( auto & track : tracks_ ) {
	unsigned int shown_frame_count = 0;
	double running_mean = 0.0;
	double running_varsum = 0.0;
	float running_min = 1.0;
	double time_margin_required = 0;

	/* extrapolate if we only have a partial track so far */
	if ( (not track.empty()) and (track.back().timestamp != total_shown_frame_count_ - 1) ) {
	  const double average_bytes_per_shown_frame = double( track.back().cumulative_length ) / double( track.back().timestamp );
	  const unsigned int num_shown_frames_remaining = total_shown_frame_count_ - track.back().timestamp;
	  /*
	  cerr << "frames remaining: " << num_shown_frames_remaining << endl;
	  cerr << "current bytes per second: " << estimated_bytes_per_second << endl;
	  */
	  time_margin_required = average_bytes_per_shown_frame * num_shown_frames_remaining / estimated_bytes_per_second;
	  time_margin_required -= num_shown_frames_remaining / 24.0;
	  //	  cerr << "estimate this will take: " << time_margin_required << endl;
	  if ( time_margin_required < 0.0 ) { time_margin_required = 0.0; }
	}
	
	/* algorithm from Knuth volume 2, per http://www.johndcook.com/blog/standard_deviation/ */
	for ( auto frame = track.rbegin(); frame != track.rend(); frame++ ) {
	  if ( frame->shown ) {
	    shown_frame_count++;
	  
	    if ( shown_frame_count == 0 ) {
	      frame->average_quality_to_end = 1;
	      frame->stddev_quality_to_end = 0;
	      frame->min_quality_to_end = 1;
	    } else if ( shown_frame_count == 1 ) {
	      running_mean = running_min = frame->quality;
	      frame->average_quality_to_end = running_mean;
	      frame->stddev_quality_to_end = 0;
	      frame->min_quality_to_end = running_min;
	    } else {
	      const double new_mean = running_mean + ( frame->quality - running_mean ) / shown_frame_count;
	      const double new_varsum = running_varsum + ( frame->quality - running_mean ) * ( frame->quality - new_mean );
	      tie( running_mean, running_varsum ) = make_tuple( new_mean, new_varsum );

	      frame->average_quality_to_end = running_mean;
	      frame->stddev_quality_to_end = sqrt( running_varsum / ( shown_frame_count - 1.0 ) );

	      running_min = min( running_min, frame->quality );
	      frame->min_quality_to_end = running_min;
	    }
	  } else {
	    frame->average_quality_to_end = running_mean;
	    frame->stddev_quality_to_end = sqrt( running_varsum / ( shown_frame_count - 1.0 ) );
	    frame->min_quality_to_end = running_min;
	  }

	  if ( frame_store.find( frame->offset ) == frame_store.end() ) {
	    /* would need to fetch */
	    frame->time_to_fetch = frame->length / estimated_bytes_per_second;
	  } else {
	    frame->time_to_fetch = 0;
	  }

	  time_margin_required += frame->time_to_fetch;
	  frame->time_margin_required = time_margin_required;

	  if ( frame->shown ) {
	    time_margin_required -= ( 1.0 / 24.0 );
	    if ( time_margin_required < 0.0 ) { time_margin_required = 0.0; }
	  }
	}
      }

      analysis_generation_++;
    }, estimated_bytes_per_second_, move( frame_store_ ) );

  newthread.detach();
}

void VideoMap::report_feasibility() const
{
  unique_lock<mutex> lock { mutex_ };
  for ( unsigned int i = 0; i < tracks_.size(); i++ ) {
    if ( tracks_[ i ].empty() ) {
      continue;
    }

    const auto & frame = tracks_[ i ].front();
    
    cerr << "track " << i << ", average quality: " << frame.average_quality_to_end
	 << ", stddev: " << frame.stddev_quality_to_end
	 << ", min: " << frame.min_quality_to_end
	 << ", timestamp = " << frame.timestamp
	 << ", time margin required: " << frame.time_margin_required
	 << "\n";
  }
}
