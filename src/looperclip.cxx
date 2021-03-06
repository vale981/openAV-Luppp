/*
 * Author: Harry van Haaren 2013
 *         harryhaaren@gmail.com
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdio.h>
#include "config.hxx"
#include "jack.hxx"
#include "event.hxx"
#include "eventhandler.hxx"
#include "audiobuffer.hxx"

#include "controllerupdater.hxx"
#include "timemanager.hxx"
#include <math.h>


extern Jack* jack;

LooperClip::LooperClip(int t, int s) :
	Stately(),
	track(t),
	scene(s)
{
	_buffer = new AudioBuffer();
	_buffer->nonRtResize( 4410 );
	init();
}

void LooperClip::init()
{
	_loaded     = false;
	_playing    = false;
	_recording  = false;

	_queuePlay  = false;
	_queueStop  = false;
	_queueRecord= false;
	_beatsToRecord= -1;
	setBeats(0);

	if ( _buffer ) {
		_buffer->init();
	}
	_newBufferInTransit = false;

	_playhead   = 0;
	_recordhead = 0;
}

void LooperClip::save()
{
	// ensure the buffer exists, and is saveable (not recording)
	if ( _loaded && !_recording && !_queueRecord ) {
		char buffer [50];
		sprintf (buffer, "LC::save() track %i, scene %i", track,scene);
		EventGuiPrint e( buffer );
		writeToGuiRingbuffer( &e );

		int frames = _buffer->getAudioFrames();
		EventRequestSaveBuffer e2( track, scene, frames );
		writeToGuiRingbuffer( &e2 );
	} else {
		// notify of "success" of save if there *is* no state to save
		Stately::success();
	}
}

void LooperClip::reset()
{
	// TODO make the LooperClip reset to initial state
	if ( _loaded ) {
		char buffer [50];
		sprintf (buffer, "LC::reset() track %i, scene %i", track,scene);
		EventGuiPrint e( buffer );
		writeToGuiRingbuffer( &e );

		// set "progress" to zero as there's no clip anymore
		jack->getControllerUpdater()->setTrackSceneProgress(track, scene, 0 );
	} else {
		//SaveAble::done();
	}

	init();
}

/// loads a sample: eg from disk, unloading current sample if necessary
void LooperClip::load( AudioBuffer* ab )
{
	_loaded = true;
	_recording = false;
	_playing    = false;

	_queuePlay  = false;
	_queueStop  = false;
	_queueRecord= false;

	if ( _buffer ) {
		EventDeallocateBuffer e( _buffer );
		writeToGuiRingbuffer( &e );
	}

	_buffer = ab;
	EventClipBeatsChanged e( track, scene, _beatsToRecord, true );
	writeToGuiRingbuffer( &e );

	_playhead = 0;
	jack->getControllerUpdater()->setTrackSceneProgress(track, scene, 0 );

	// set the endpoint to the buffer's size
	_recordhead = _buffer->getSize();

#ifdef DEBUG_BUFFER
	char buffer [50];
	sprintf (buffer, "LC::load() t %i, s %i, aF %i",track, scene, int(_buffer->getAudioFrames()) );
	EventGuiPrint e( buffer );
	writeToGuiRingbuffer( &e );
#endif
}

void LooperClip::setRequestedBuffer( AudioBuffer* ab )
{
	if ( _buffer ) {
		size_t size = _buffer->getSize();
		memcpy( &ab->getDataL().at(0), &_buffer->getDataL().at(0), sizeof(float)*size);
		memcpy( &ab->getDataR().at(0), &_buffer->getDataR().at(0), sizeof(float)*size);

		ab->setID   ( _buffer->getID()    );
		ab->setBeats( _buffer->getBeats() );

		EventDeallocateBuffer e( _buffer );
		writeToGuiRingbuffer( &e );
	}

	_buffer = ab;

	_newBufferInTransit = false;
}



void LooperClip::recieveSaveBuffer( AudioBuffer* saveBuffer )
{
	if ( saveBuffer->getSize() >= _buffer->getDataL().at(0) ||
	     saveBuffer->getSize() >= _buffer->getDataR().at(0) ) {
		// copy current contents into save buffer,
		// getData() contains L and R buffer, so twice the size is needed
		size_t framesBySize = _buffer->getAudioFrames();
		memcpy( &saveBuffer->getDataL().at(0), &_buffer->getDataL().at(0), sizeof(float)*framesBySize);
		memcpy( &saveBuffer->getDataR().at(0), &_buffer->getDataR().at(0), sizeof(float)*framesBySize);

		saveBuffer->setID   ( _buffer->getID()    );
		saveBuffer->setBeats( _buffer->getBeats() );
		saveBuffer->setAudioFrames( _buffer->getAudioFrames() );

		EventStateSaveBuffer e ( track, scene, saveBuffer );
		writeToGuiRingbuffer( &e );

		Stately::success();
	} else {
		char buffer [50];
		sprintf (buffer, "LC:: %i, %i: can't save, buf too small",track, scene );
		EventGuiPrint e( buffer );
		writeToGuiRingbuffer( &e );
		Stately::error("");
	}
}

void LooperClip::setPlayHead(float ph)
{
	if(!_recording&&_playing) {
		_playhead = ph;
		jack->getControllerUpdater()->setTrackSceneProgress( track, scene, getProgress() );
	}
}

void LooperClip::setBarsToRecord(int bars)
{
	if(!(_playing || _queuePlay || _queueStop || _loaded)) {
		_beatsToRecord = bars * 4; // we set beats
		EventClipBeatsChanged e( track, scene, _beatsToRecord, true);
		writeToGuiRingbuffer(&e);
	}
}

int LooperClip::getBeatsToRecord()
{
	return _beatsToRecord;
}

int LooperClip::getBarsToRecord(){
	return _beatsToRecord / 4;
}

void LooperClip::record(int count, float* L, float* R)
{
	// write "count" samples into current buffer.
	if ( _buffer ) {
		size_t size = _buffer->getSize();

		for(int i = 0; i < count; i++) {
			if ( _recordhead < size ) {
				_buffer->getDataL().at( _recordhead ) = *L++;
				_buffer->getDataR().at( _recordhead ) = *R++;
				_recordhead++;
			} else {
				// break: this is *BAD*, audio data is lost but the buffer isn't here
				// yet to hold new audio data so there's no option. This has not been
				// encountered in actual usage, only during the development process.
				char buffer [50];
				sprintf (buffer, "LooperClip t %i, s %i, Error: out of mem!",track, scene );
				EventGuiPrint e( buffer );
				writeToGuiRingbuffer( &e );
#ifdef BUILD_TESTS
				LUPPP_WARN("%s","buffer has no space");
#endif

				break;
			}
		}
	}

	_loaded = true;
}

unsigned long LooperClip::recordSpaceAvailable()
{
	if ( _buffer )
		// getData() contains L and R buffer, so it is twice the size
		return _buffer->getSize() - _recordhead;

	return 0;
}

size_t LooperClip::audioBufferSize()
{
	if ( _buffer ) {
		return _buffer->getSize();
	}
	return 0;
}

void LooperClip::setBeats(int beats)
{
	if (_loaded || _playing || _queuePlay || _queueStop || beats == 0) {
		if(_buffer)
			_buffer->setBeats( beats );

		EventClipBeatsChanged e(track, scene, beats, false);
		writeToGuiRingbuffer(&e);
	}
}

int LooperClip::getBeats()
{
	if ( _buffer )
		return _buffer->getBeats();

	return 0;
}

long LooperClip::getBufferLenght()
{
	return _recordhead;
}

long LooperClip::getActualAudioLength()
{
	char cbuffer [50];
//    sprintf (cbuffer, "LooperClip recordhead %f,audioFrames %d \n",_recordhead,(int)_buffer->getAudioFrames());
//    EventGuiPrint e( cbuffer );
//    writeToGuiRingbuffer( &e );
//    printf(cbuffer);
	return _buffer->getAudioFrames();
}

void LooperClip::bar()
{
	bool change = false;
	GridLogic::State s = GridLogic::STATE_EMPTY;

	// first update the buffer, as time has passed
	if ( _recording ) {
		// FIXME: assumes 4 beats in a bar
		setBeats( _buffer->getBeats() + 4 );
		_buffer->setAudioFrames( jack->getTimeManager()->getFpb() * _buffer->getBeats() );
	}

	if ( _playhead >= _recordhead ) {
		_playhead = 0.f;
	}

	if ( _queuePlay && _loaded ) {
		//LUPPP_NOTE("QPLay + loaded" );
		_playing = true;
		s = GridLogic::STATE_PLAYING;
		_recording = false;
		_queuePlay = false;
		change = true;

		_playhead = 0;
	} else if ( _queueStop && _loaded ) {
		_playing   = false;
		s = GridLogic::STATE_STOPPED;
		_recording = false;
		_queueStop = false;
		change = true;
		// set "progress" to zero, as we're stopped!
		jack->getControllerUpdater()->setTrackSceneProgress(track, scene, 0 );
	} else if ( _queueRecord ) {
		_recording   = true;
		s = GridLogic::STATE_RECORDING;
		_playing     = false;
		_queueRecord = false;
		change = true;

		if ( _buffer ) {
			_buffer->setBeats( 0 );
		}

		_recordhead = 0;
	} else if ( _queuePlay ) {
		// clip was queued, but there's nothing loaded
		_queuePlay = false;
		change = true;
	}

	if ( change ) {
		jack->getControllerUpdater()->setSceneState(track, scene, s );
	}
}

void LooperClip::neutralize()
{
	_queuePlay = false;
	_queueRecord = false;
	_queueStop = false;
}

bool LooperClip::somethingQueued()
{
	if ( _queuePlay || _queueStop || _queueRecord ) {
		return true;
	}
	return false;
}

void LooperClip::queuePlay(bool qP)
{
	_queuePlay   = true;
	_queueStop   = false;
	_queueRecord = false;
}

void LooperClip::queueStop()
{
	// comment
	if ( _loaded ) {
		_queueStop   = true;
		_queuePlay   = false;
	}
}

void LooperClip::queueRecord()
{
	_queueRecord = true;
	_queuePlay   = false;
	_queueStop   = false;
}

GridLogic::State LooperClip::getState()
{
	GridLogic::State s = GridLogic::STATE_EMPTY;

	if ( _loaded       )
		s = GridLogic::STATE_STOPPED;
	if ( _playing      )
		s = GridLogic::STATE_PLAYING;
	if ( _recording    )
		s = GridLogic::STATE_RECORDING;
	if ( _queuePlay    )
		s = GridLogic::STATE_PLAY_QUEUED;
	if ( _queueStop    )
		s = GridLogic::STATE_STOP_QUEUED;
	if ( _queueRecord  )
		s = GridLogic::STATE_RECORD_QUEUED;

	return s;
}

bool LooperClip::playing()
{
	return _playing;
}

bool LooperClip::getQueueStop()
{
	return _queueStop;
}

bool LooperClip::getQueuePlay()
{
	return _queuePlay;
}

bool LooperClip::getLoaded()
{
	return _loaded;
}

bool LooperClip::recording()
{
	return _recording;
}

void LooperClip::newBufferInTransit(bool n)
{
	_newBufferInTransit = n;
}

bool LooperClip::newBufferInTransit()
{
	return _newBufferInTransit;
}

void LooperClip::getSample(float playSpeed, float* L, float* R)
{
	if ( _buffer && (_buffer->getSize() > 0)) {
		if ( _playhead >= _recordhead ||
		     _playhead >= _buffer->getSize() ||
		     _playhead < 0  ) {
			_playhead = 0;

			EventGuiPrint e( "LooperClip resetting _playhead" );
			//writeToGuiRingbuffer( &e );
		}

		std::vector<float>& vL = _buffer->getDataL();
		std::vector<float>& vR = _buffer->getDataR();
		*L = vL[_playhead];
		*R = vR[_playhead];
		_playhead += playSpeed;
	} else {
		*L = 0.f;
		*R = 0.f;
	}
}

float LooperClip::getProgress()
{
	if ( _buffer && _playing ) {
		float p = float(_playhead) / _recordhead;
		//printf("LooperClip progress %f\n", p );
		return p;
	}
	return 0.f;
}

float LooperClip::getPlayhead()
{
	return _playhead;
}

#ifdef BUILD_TESTS
void LooperClip::setState( bool load, bool play, bool rec, bool qPlay, bool qStop, bool qRec )
{
	_loaded       = load;
	_playing      = play;
	_recording    = rec;

	_queuePlay    = qPlay;
	_queueStop    = qStop;
	_queueRecord  = qRec;
}
#endif
