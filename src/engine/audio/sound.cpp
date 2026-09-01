/*-------------------------------------------------------------------------------

	BARONY
	File: sound.cpp
	Desc: various sound functions

	Copyright 2013-2016 (c) Turning Wheel LLC, all rights reserved.
	See LICENSE for details.

-------------------------------------------------------------------------------*/

#include "../../main.hpp"
#include "../../files.hpp"
#include "../../game.hpp"
#include "sound.hpp"
#include <atomic>
#include <cctype>
#include <limits>
#include <vector>
#ifndef EDITOR
#include "../../player.hpp"
#endif

#ifdef USE_FMOD
#include "fmod_errors.h"
#elif defined USE_OPENAL
#ifdef USE_TREMOR
#include <tremor/ivorbisfile.h>
#else
#include <ogg/ogg.h>
#include <vorbis/vorbisfile.h>
#include <vorbis/codec.h>
#endif
#endif

#ifdef USE_FMOD
#elif defined USE_OPENAL
#else
void setGlobalVolume(real_t master, real_t music, real_t gameplay, real_t ambient, real_t environment, real_t notification)
{
	return;
}
void setAudioDevice(const std::string& device) 
{
	return;
}
#endif

#ifdef USE_FMOD

bool FMODErrorCheck()
{
	if (no_sound)
	{
		return false;
	}
	if (fmod_result != FMOD_OK)
	{
		printlog("[FMOD Error] Error Code (%d): \"%s\"\n", fmod_result, FMOD_ErrorString(fmod_result)); //Report the FMOD error.
		return true;
	}

	return false;
}

void setAudioDevice(const std::string& device) {
	int selected_driver = 0;
	int numDrivers = 0;
	fmod_system->getNumDrivers(&numDrivers);
	for (int i = 0; i < numDrivers; ++i) {
		FMOD_GUID guid;
		fmod_result = fmod_system->getDriverInfo(i, nullptr, 0, &guid, nullptr, nullptr, nullptr);

		uint32_t _1; memcpy(&_1, &guid.Data1, sizeof(_1));
		uint64_t _2; memcpy(&_2, &guid.Data4, sizeof(_2));
		char guid_string[25];
		snprintf(guid_string, sizeof(guid_string), FMOD_AUDIO_GUID_FMT, _1, _2);
		if (!selected_driver && device == guid_string) {
			selected_driver = i;
		}
	}
	fmod_system->setDriver(selected_driver);
}

void setRecordDevice(const std::string& device)
{
#ifndef EDITOR
	int selected_driver = 0;
	int numDrivers = 0;
	fmod_system->getRecordNumDrivers(&numDrivers, nullptr);
	for ( int i = 0; i < numDrivers; ++i ) {
		FMOD_GUID guid;
		constexpr int driverNameLen = 64;
		char driverName[driverNameLen] = "";
		fmod_result = fmod_system->getRecordDriverInfo(i, driverName, driverNameLen, &guid, nullptr, nullptr, nullptr, nullptr);
		if ( strstr(driverName, "[loopback]") )
		{
			continue;
		}
		uint32_t _1; memcpy(&_1, &guid.Data1, sizeof(_1));
		uint64_t _2; memcpy(&_2, &guid.Data4, sizeof(_2));
		char guid_string[25];
		snprintf(guid_string, sizeof(guid_string), FMOD_AUDIO_GUID_FMT, _1, _2);
		if ( !selected_driver && device == guid_string ) {
			selected_driver = i;
		}
	}
	VoiceChat.setRecordingDevice(selected_driver);
#endif
}

void setGlobalVolume(real_t master, real_t music, real_t gameplay, real_t ambient, real_t environment, real_t notification) {
    master = std::min(std::max(0.0, master), 1.0);
    music = std::min(std::max(0.0, music / 4.0), 1.0); // music volume cut in half because the music is loud...
    gameplay = std::min(std::max(0.0, gameplay), 1.0);
    ambient = std::min(std::max(0.0, ambient), 1.0);
    environment = std::min(std::max(0.0, environment), 1.0);
	notification = std::min(std::max(0.0, notification), 1.0);

	music_group->setVolume(master * music);
	sound_group->setVolume(master * gameplay);
	soundAmbient_group->setVolume(master * ambient);
	soundEnvironment_group->setVolume(master * environment);
	music_notification_group->setVolume(master * notification);
	soundNotification_group->setVolume(master * notification);
	music_ensemble_global_send_group->setVolume(1.f);

#ifndef EDITOR
	ensembleSounds.ensemble_recv_global_volume = master * (music * 4);
	ensembleSounds.ensemble_recv_player_volume = master * gameplay;
	if ( VoiceChat.outChannelGroup )
	{
		VoiceChat.outChannelGroup->setVolume(master);
	}
#endif
}

#ifndef EDITOR
	static ConsoleVariable<float> cvar_sfx_notification_music_fade("/sfx_notification_music_fade", 0.5f);
	static ConsoleVariable<float> cvar_sfx_ensemble_music_fade("/sfx_ensemble_music_fade", 0.f);
#endif // !EDITOR

void sound_update(int player, int index, int numplayers)
{
#ifdef DEBUG_EVENT_TIMERS
	auto time1 = std::chrono::high_resolution_clock::now();
	auto time2 = std::chrono::high_resolution_clock::now();
	auto accum = 1000 * std::chrono::duration_cast<std::chrono::duration<double>>(time2 - time1).count();
	if ( accum > 5 )
	{
		printlog("Large tick time: [10] %f", accum);
	}
	time1 = std::chrono::high_resolution_clock::now();
#endif

	if (no_sound)
	{
		return;
	}
	if (!fmod_system)
	{
		return;
	}

	FMOD_VECTOR position, forward, up;
	bool playing = false;

	auto& camera = cameras[index];

	position.x = (float)(camera.x);
	position.y = (float)(camera.z / (real_t)32.0);
	position.z = (float)(camera.y);

	/*forward.x = -1.0 * cos(camera.ang) * cos(camera.vang);
	forward.y =  1.0 * sin(camera.vang);
	forward.z = -1.0 * sin(camera.ang) * cos(camera.vang);*/
 
    forward.x = (float)((real_t)1.0 * cos(camera.ang));
    forward.y = 0.f;
    forward.z = (float)((real_t)1.0 * sin(camera.ang));

	/*up.x = -1.0 * cos(camera.ang) * sin(camera.vang);
	up.y =  1.0 * cos(camera.vang);
	up.z = -1.0 * sin(camera.ang) * sin(camera.vang);*/
    up.x = 0.f;
    up.y = 1.f;
    up.z = 0.f;

	//FMOD_System_Set3DListenerAttributes(fmod_system, 0, &position, &velocity, &forward, &up);
	fmod_system->set3DNumListeners(numplayers);

#ifdef DEBUG_EVENT_TIMERS
	time2 = std::chrono::high_resolution_clock::now();
	accum = 1000 * std::chrono::duration_cast<std::chrono::duration<double>>(time2 - time1).count();
	if ( accum > 5 )
	{
		printlog("Large tick time: [11] %f", accum);
	}
	time1 = std::chrono::high_resolution_clock::now();
#endif

	fmod_system->set3DListenerAttributes(player, &position, nullptr, &forward, &up);

#ifdef DEBUG_EVENT_TIMERS
	time2 = std::chrono::high_resolution_clock::now();
	accum = 1000 * std::chrono::duration_cast<std::chrono::duration<double>>(time2 - time1).count();
	if ( accum > 5 )
	{
		printlog("Large tick time: [12] %f", accum);
	}
	time1 = std::chrono::high_resolution_clock::now();
#endif

	if (player == 0) {
#ifndef EDITOR
		//Fade in the currently playing music.
		bool notificationPlaying = false;
		if ( music_notification_group )
		{
			music_notification_group->isPlaying(&notificationPlaying);
		}
		bool ensemblePlaying = false;
		if ( music_ensemble_global_send_group )
		{
			music_ensemble_global_send_group->isPlaying(&ensemblePlaying);
			if ( ensemblePlaying )
			{
				bool ensemblePaused = false;
				music_ensemble_global_send_group->getPaused(&ensemblePaused); // if playing, then check if paused
				if ( ensemblePaused )
				{
					ensemblePlaying = false;
				}
				else
				{
					Uint32 globalEnsemblePlaying = 0;
					Uint32 localEnsemblePlaying = 0;
					for ( int i = 0; i < MAXPLAYERS; ++i )
					{
						if ( players[i]->isLocalPlayerAlive() )
						{
							globalEnsemblePlaying |= (players[i]->mechanics.ensembleDataUpdate >> 16) & 0xFFFF;
							localEnsemblePlaying |= (players[i]->mechanics.ensembleDataUpdate >> 8) & 0xFF;
						}
						/*if ( players[i]->entity && !client_disconnected[i] )
						{
							// if we want other players to override the main soundtrack with local sound
							localEnsemblePlaying |= (players[i]->mechanics.ensembleDataUpdate >> 8) & 0xFF;
						}*/
					}
					if ( globalEnsemblePlaying == 0 || (*cvar_ensemble_vol_bg <= -79.f && localEnsemblePlaying == 0)
						|| (!instrument_bg_enabled && localEnsemblePlaying == 0) )
					{
						ensemblePlaying = false;
					}
				}
			}
		}
#endif

#ifdef DEBUG_EVENT_TIMERS
		time2 = std::chrono::high_resolution_clock::now();
		accum = 1000 * std::chrono::duration_cast<std::chrono::duration<double>>(time2 - time1).count();
		if ( accum > 5 )
		{
			printlog("Large tick time: [13] %f", accum);
		}
		time1 = std::chrono::high_resolution_clock::now();
#endif

		if (music_channel)
		{
			playing = false;
			music_channel->isPlaying(&playing);
			if (playing)
			{
				float volume = 1.0f;
				music_channel->getVolume(&volume);

#ifdef DEBUG_EVENT_TIMERS
				time2 = std::chrono::high_resolution_clock::now();
				accum = 1000 * std::chrono::duration_cast<std::chrono::duration<double>>(time2 - time1).count();
				if ( accum > 5 )
				{
					printlog("Large tick time: [14] %f", accum);
				}
				time1 = std::chrono::high_resolution_clock::now();
#endif
#ifdef EDITOR
				if ( volume < 1.0f )
				{
					volume += fadein_increment * 2;
					if ( volume > 1.0f )
					{
						volume = 1.0f;
					}
					music_channel->setVolume(volume);
				}
#else
				if ( notificationPlaying && volume > 0.0f )
				{
					volume -= fadeout_increment * 5;
					if ( volume < *cvar_sfx_notification_music_fade )
					{
						volume = *cvar_sfx_notification_music_fade;
					}
					music_channel->setVolume(volume);
				}
				else if ( ensemblePlaying )
				{
					volume -= fadeout_increment * 5;
					if ( volume < *cvar_sfx_ensemble_music_fade )
					{
						volume = *cvar_sfx_ensemble_music_fade;
					}
					music_channel->setVolume(volume);
				}
				else if (volume < 1.0f)
				{
					volume += fadein_increment * 2;
					if (volume > 1.0f)
					{
						volume = 1.0f;
					}
					music_channel->setVolume(volume);
				}
#endif
#ifdef DEBUG_EVENT_TIMERS
				time2 = std::chrono::high_resolution_clock::now();
				accum = 1000 * std::chrono::duration_cast<std::chrono::duration<double>>(time2 - time1).count();
				if ( accum > 5 )
				{
					printlog("Large tick time: [15] %f", accum);
				}
				time1 = std::chrono::high_resolution_clock::now();
#endif
			}
		}

		//The following makes crossfading possible. Fade out the last playing music. //TODO: Support for saving music so that it can be resumed (for stuff interrupting like combat music).
		if (music_channel2)
		{
			playing = false;

#ifdef DEBUG_EVENT_TIMERS
			time2 = std::chrono::high_resolution_clock::now();
			accum = 1000 * std::chrono::duration_cast<std::chrono::duration<double>>(time2 - time1).count();
			if ( accum > 5 )
			{
				printlog("Large tick time: [16] %f", accum);
			}
			time1 = std::chrono::high_resolution_clock::now();
#endif

			music_channel2->isPlaying(&playing);
			if (playing)
			{
				float volume = 0.0f;
				music_channel2->getVolume(&volume);

#ifdef DEBUG_EVENT_TIMERS
				time2 = std::chrono::high_resolution_clock::now();
				accum = 1000 * std::chrono::duration_cast<std::chrono::duration<double>>(time2 - time1).count();
				if ( accum > 5 )
				{
					printlog("Large tick time: [17] %f", accum);
				}
				time1 = std::chrono::high_resolution_clock::now();
#endif

				if (volume > 0.0f)
				{
					volume -= fadeout_increment * 2;
					if (volume < 0.0f)
					{
						volume = 0.0f;
					}
					music_channel2->setVolume(volume);
				}

#ifdef DEBUG_EVENT_TIMERS
				time2 = std::chrono::high_resolution_clock::now();
				accum = 1000 * std::chrono::duration_cast<std::chrono::duration<double>>(time2 - time1).count();
				if ( accum > 5 )
				{
					printlog("Large tick time: [18] %f", accum);
				}
				time1 = std::chrono::high_resolution_clock::now();
#endif
			}
		}
	}

#ifdef DEBUG_EVENT_TIMERS
	time2 = std::chrono::high_resolution_clock::now();
	accum = 1000 * std::chrono::duration_cast<std::chrono::duration<double>>(time2 - time1).count();
	if ( accum > 5 )
	{
		printlog("Large tick time: [19] %f", accum);
	}
	time1 = std::chrono::high_resolution_clock::now();
#endif

	if (player == numplayers - 1) {
#ifndef EDITOR
		VoiceChat.update();
#endif
		fmod_system->update();
	}

#ifdef DEBUG_EVENT_TIMERS
	time2 = std::chrono::high_resolution_clock::now();
	accum = 1000 * std::chrono::duration_cast<std::chrono::duration<double>>(time2 - time1).count();
	if ( accum > 5 )
	{
		printlog("Large tick time: [20] %f", accum);
	}
	time1 = std::chrono::high_resolution_clock::now();
#endif
}

#elif defined USE_OPENAL

struct OPENAL_BUFFER {
	ALuint id;
	bool stream;
	bool loop;
	bool persistent_channel;
	char oggfile[PATH_MAX];
};
struct OPENAL_SOUND {
	ALuint id;
	OPENAL_CHANNELGROUP *group;
	float volume;
	OPENAL_BUFFER *buffer;
	bool active;
	char* oggdata;
	int oggdata_length;
	int ogg_seekoffset;
	OggVorbis_File oggStream;
	vorbis_info* vorbisInfo;
	vorbis_comment* vorbisComment;
	ALuint streambuff[4];
	bool loop;
	bool stream_active;
	bool stream_open;
	int indice;
};

struct OPENAL_CHANNELGROUP {
	float volume;
	int num;
	int cap;
	OPENAL_SOUND **sounds;
};

SDL_mutex *openal_mutex;

static size_t openal_oggread(void* ptr, size_t size, size_t nmemb, void* datasource) {
	OPENAL_SOUND* self = (OPENAL_SOUND*)datasource;
	if (!self || !ptr || size == 0 || nmemb == 0 || !self->oggdata) {
		return 0;
	}
	const size_t offset = static_cast<size_t>(self->ogg_seekoffset);
	const size_t length = static_cast<size_t>(self->oggdata_length);
	const size_t availableItems = offset < length ? (length - offset) / size : 0U;
	const size_t itemCount = std::min(nmemb, availableItems);
	const size_t bytes = itemCount * size;
	memcpy(ptr, self->oggdata + offset, bytes);
	self->ogg_seekoffset += static_cast<int>(bytes);
	return itemCount;
}

static int openal_oggseek(void* datasource, ogg_int64_t offset, int whence) {
	OPENAL_SOUND* self = (OPENAL_SOUND*)datasource;
	if (!self) {
		return -1;
	}
	ogg_int64_t seek_offset = 0;

	switch(whence) {
	case SEEK_CUR:
		seek_offset = self->ogg_seekoffset + offset;
		break;
	case SEEK_END:
		seek_offset = self->oggdata_length + offset;
		break;
	case SEEK_SET:
		seek_offset = offset;
		break;
	default:
		return -1;
	}
	if(seek_offset < 0 || seek_offset > self->oggdata_length) return -1;

	self->ogg_seekoffset = static_cast<int>(seek_offset);
	return 0;
}

static int openal_oggclose(void* datasource) {
	return 0;
}

static long int openal_oggtell(void* datasource) {
	OPENAL_SOUND* self = (OPENAL_SOUND*)datasource;
	return self ? self->ogg_seekoffset : -1;
}

static int openal_oggopen(OPENAL_SOUND *self, const char* oggfile) {
	if (!self || !oggfile) {
		return 0;
	}
	File *f = openDataFile(oggfile, "rb");

	ov_callbacks oggcb = {openal_oggread, openal_oggseek, openal_oggclose, openal_oggtell};

	if(!f) {
		return 0;
	}

	const size_t fileSize = f->size();
	if (fileSize == 0 || fileSize > static_cast<size_t>(std::numeric_limits<int>::max())) {
		FileIO::close(f);
		printlog("[OpenAL]: invalid OGG stream size for '%s'.", oggfile);
		return 0;
	}
	self->ogg_seekoffset = 0;
	self->oggdata_length = static_cast<int>(fileSize);
	self->oggdata = (char*)malloc(fileSize);
	if (!self->oggdata || f->read(self->oggdata, sizeof(char), fileSize) != fileSize) {
		free(self->oggdata);
		self->oggdata = nullptr;
		self->oggdata_length = 0;
		FileIO::close(f);
		printlog("[OpenAL]: failed to read OGG stream '%s'.", oggfile);
		return 0;
	}
	FileIO::close(f);

	if(ov_open_callbacks(self, &self->oggStream, 0, 0, oggcb)) {
		printlog("[OpenAL]: invalid OGG stream '%s'.", oggfile);
		free(self->oggdata);
		self->oggdata = nullptr;
		self->oggdata_length = 0;
		return 0;
	}
	self->stream_open = true;

	self->vorbisInfo = ov_info(&self->oggStream, -1);
	self->vorbisComment = ov_comment(&self->oggStream, -1);
	if (!self->vorbisInfo || self->vorbisInfo->channels < 1
		|| self->vorbisInfo->channels > 2 || self->vorbisInfo->rate <= 0) {
		printlog("[OpenAL]: unsupported OGG stream format in '%s'.", oggfile);
		ov_clear(&self->oggStream);
		free(self->oggdata);
		self->oggdata = nullptr;
		self->oggdata_length = 0;
		self->stream_open = false;
		return 0;
	}

	alGetError();
	alGenBuffers(4, self->streambuff);
	if (alGetError() != AL_NO_ERROR) {
		printlog("[OpenAL]: failed to allocate stream buffers for '%s'.", oggfile);
		ov_clear(&self->oggStream);
		free(self->oggdata);
		self->oggdata = nullptr;
		self->oggdata_length = 0;
		self->stream_open = false;
		return 0;
	}
	return 1;
}

static int openal_oggrelease(OPENAL_SOUND *self) {
	if (!self || !self->stream_open) {
		return 0;
	}
	alSourceStop(self->id);
	ov_raw_seek(&self->oggStream, 0);
	int queued;
	alGetSourcei(self->id, AL_BUFFERS_QUEUED, &queued);
	while(queued--) {
		ALuint buffer;
		alSourceUnqueueBuffers(self->id, 1, &buffer);
	}
	alDeleteBuffers(4, self->streambuff);
	ov_clear(&self->oggStream);
	free(self->oggdata);
	self->oggdata = nullptr;
	self->oggdata_length = 0;
	self->ogg_seekoffset = 0;
	self->vorbisInfo = nullptr;
	self->vorbisComment = nullptr;
	self->stream_open = false;
	return 1;
}

static int openal_streamread(OPENAL_SOUND *self, ALuint buffer) {
	#define OGGSIZE 65536
	char pcm[OGGSIZE];
	int size = 0;
	int section;
	int result;
	bool rewoundWithoutData = false;

	while (size < OGGSIZE) {
		#ifdef USE_TREMOR
		result = ov_read(&self->oggStream, pcm+size, OGGSIZE -size, &section);
		#else
		result = ov_read(&self->oggStream, pcm+size, OGGSIZE -size, 0, 2, 1, &section);
		#endif
		if(result>0) {
			size += result;
			rewoundWithoutData = false;
		} else if (result == 0 && self->loop && !rewoundWithoutData
			&& ov_raw_seek(&self->oggStream, 0) == 0) {
			rewoundWithoutData = true;
			continue;
		} else {
			break;
		}
	}

	if(size==0) {
		return 0;
	}
	alBufferData(buffer, 
		(self->vorbisInfo->channels==1)?AL_FORMAT_MONO16:AL_FORMAT_STEREO16, 
		pcm, size, self->vorbisInfo->rate);

	return 1;

	#undef OGGSIZE
}

static int openal_streamupdate(OPENAL_SOUND* self) {
	int processed = 0;

	alGetSourcei(self->id, AL_BUFFERS_PROCESSED, &processed);

	while(processed--) {
		ALuint buffer;

		alSourceUnqueueBuffers(self->id, 1, &buffer);

		if(openal_streamread(self, buffer))
			alSourceQueueBuffers(self->id, 1, &buffer);
	}
	ALint state = AL_STOPPED;
	ALint queued = 0;
	alGetSourcei(self->id, AL_SOURCE_STATE, &state);
	alGetSourcei(self->id, AL_BUFFERS_QUEUED, &queued);
	self->stream_active = queued > 0;
	if (state != AL_PLAYING && state != AL_PAUSED && queued > 0) {
		alSourcePlay(self->id);
	}

	return self->stream_active ? 1 : 0;
}

bool sfxUseDynamicAmbientVolume = true;
bool sfxUseDynamicEnvironmentVolume = true;

ALCcontext *openal_context = nullptr;
ALCdevice  *openal_device = nullptr;

void setAudioDevice(const std::string& device)
{
    // OpenAL device changes require rebuilding the active context. Keep the
    // current working device until that transition can be performed safely.
    (void)device;
}

void setRecordDevice(const std::string& device)
{
    // Voice recording is an FMOD-only feature in the current OpenAL backend.
    (void)device;
}

//#define openal_maxchannels 100

OPENAL_BUFFER** sounds = nullptr;
OPENAL_BUFFER** minesmusic = NULL;
OPENAL_BUFFER** swampmusic = NULL;
OPENAL_BUFFER** labyrinthmusic = NULL;
OPENAL_BUFFER** ruinsmusic = NULL;
OPENAL_BUFFER** underworldmusic = NULL;
OPENAL_BUFFER** hellmusic = NULL;
OPENAL_BUFFER** intromusic = NULL;
OPENAL_BUFFER* intermissionmusic = NULL;
OPENAL_BUFFER* minetownmusic = NULL;
OPENAL_BUFFER* splashmusic = NULL;
OPENAL_BUFFER* librarymusic = NULL;
OPENAL_BUFFER* shopmusic = NULL;
OPENAL_BUFFER* storymusic = NULL;
OPENAL_BUFFER** minotaurmusic = NULL;
OPENAL_BUFFER* herxmusic = NULL;
OPENAL_BUFFER* templemusic = NULL;
OPENAL_BUFFER* endgamemusic = NULL;
OPENAL_BUFFER* devilmusic = NULL;
OPENAL_BUFFER* escapemusic = NULL;
OPENAL_BUFFER* sanctummusic = NULL;
OPENAL_BUFFER* introductionmusic = NULL;
OPENAL_BUFFER** cavesmusic = NULL;
OPENAL_BUFFER** citadelmusic = NULL;
OPENAL_BUFFER* gnomishminesmusic = NULL;
OPENAL_BUFFER* greatcastlemusic = NULL;
OPENAL_BUFFER* sokobanmusic = NULL;
OPENAL_BUFFER* caveslairmusic = NULL;
OPENAL_BUFFER* bramscastlemusic = NULL;
OPENAL_BUFFER* hamletmusic = NULL;
OPENAL_BUFFER* tutorialmusic = nullptr;
OPENAL_BUFFER* gameovermusic = nullptr;
OPENAL_BUFFER* introstorymusic = nullptr;
bool levelmusicplaying = false;

OPENAL_SOUND* music_channel = nullptr;
OPENAL_SOUND* music_channel2 = nullptr;
OPENAL_SOUND* music_resume = nullptr;

OPENAL_CHANNELGROUP *sound_group = NULL;
OPENAL_CHANNELGROUP *soundAmbient_group = NULL;
OPENAL_CHANNELGROUP *soundEnvironment_group = NULL;
OPENAL_CHANNELGROUP *music_group = NULL;
OPENAL_CHANNELGROUP *music_notification_group = NULL;
OPENAL_CHANNELGROUP *soundNotification_group = NULL;

float fadein_increment = 0.002f;
float default_fadein_increment = 0.002f;
float fadeout_increment = 0.005f;
float default_fadeout_increment = 0.005f;

#define MAXSOUND 1024
OPENAL_SOUND openal_sounds[MAXSOUND];
int lower_freechannel = 0;
int upper_unfreechannel = 0;

SDL_Thread* openal_soundthread;
static std::atomic<bool> openalSoundRunning{ false };
static bool openalInitialized = false;

void OPENAL_RemoveChannelGroup(OPENAL_SOUND *channel, OPENAL_CHANNELGROUP *group);

static void private_OPENAL_Channel_Stop(OPENAL_SOUND* channel) {
	if (!channel || !channel->active) {
		return;
	}
	// stop and delete Sound (channel)
	channel->stream_active = false;
	alSourceStop(channel->id);
	if(channel->group)
		OPENAL_RemoveChannelGroup(channel, channel->group);
	if(channel->buffer && channel->buffer->stream)
		openal_oggrelease(channel);
	alDeleteSources( 1, &channel->id );
	channel->id = 0;
	channel->buffer = nullptr;
	channel->group = nullptr;
	channel->active = false;
}


int OPENAL_ThreadFunction(void* data) {
	(void)data;
	while(openalSoundRunning.load()) {
		SDL_LockMutex(openal_mutex);

		// Updates Stream channel
		for (int i=0; i<upper_unfreechannel; i++) {
			if(openal_sounds[i].active && openal_sounds[i].buffer
				&& openal_sounds[i].buffer->stream && openal_sounds[i].stream_active) {
				openal_streamupdate(&openal_sounds[i]);
			}
		}

		// check finished sound to free them, unless it's a streamed channel...
		for (int i=0; i<upper_unfreechannel; i++) {
			if(openal_sounds[i].active && openal_sounds[i].buffer
				&& !openal_sounds[i].buffer->stream
				&& !openal_sounds[i].buffer->persistent_channel) {
				ALint state = 0;
				alGetSourcei(openal_sounds[i].id, AL_SOURCE_STATE, &state);
				if(!(state==AL_PLAYING || state==AL_PAUSED || state==AL_INITIAL)) {
					private_OPENAL_Channel_Stop(&openal_sounds[i]);
					if (lower_freechannel > i)
						lower_freechannel = i;
				}
			}
		}
		while ((upper_unfreechannel > 0) && (!openal_sounds[upper_unfreechannel-1].active))
			--upper_unfreechannel;

		SDL_UnlockMutex(openal_mutex);
		
		SDL_Delay(100);
	}
	return 1;
}

int initOPENAL()
{
	if(openalInitialized)
		return 1;

	openal_device = alcOpenDevice(NULL); // preferred device
	if(!openal_device) {
		printlog("[OpenAL]: no playback device is available.");
		return 0;
	}

	openal_context = alcCreateContext(openal_device,NULL);
	if(!openal_context) {
		printlog("[OpenAL]: failed to create a playback context.");
		alcCloseDevice(openal_device);
		openal_device = nullptr;
		return 0;
	}

	if (!alcMakeContextCurrent(openal_context)) {
		printlog("[OpenAL]: failed to activate the playback context.");
		alcDestroyContext(openal_context);
		openal_context = nullptr;
		alcCloseDevice(openal_device);
		openal_device = nullptr;
		return 0;
	}

	alDistanceModel(AL_INVERSE_DISTANCE_CLAMPED);
	alDopplerFactor(2.0f);

	// creates channels groups
	sound_group = (OPENAL_CHANNELGROUP*)calloc(1, sizeof(OPENAL_CHANNELGROUP));
	soundAmbient_group = (OPENAL_CHANNELGROUP*)calloc(1, sizeof(OPENAL_CHANNELGROUP));
	soundEnvironment_group = (OPENAL_CHANNELGROUP*)calloc(1, sizeof(OPENAL_CHANNELGROUP));
	music_group = (OPENAL_CHANNELGROUP*)calloc(1, sizeof(OPENAL_CHANNELGROUP));
	music_notification_group = (OPENAL_CHANNELGROUP*)calloc(1, sizeof(OPENAL_CHANNELGROUP));
	soundNotification_group = (OPENAL_CHANNELGROUP*)calloc(1, sizeof(OPENAL_CHANNELGROUP));
	if (!sound_group || !soundAmbient_group || !soundEnvironment_group
		|| !music_group || !music_notification_group || !soundNotification_group) {
		printlog("[OpenAL]: failed to allocate channel groups.");
		free(sound_group);
		free(soundAmbient_group);
		free(soundEnvironment_group);
		free(music_group);
		free(music_notification_group);
		free(soundNotification_group);
		sound_group = soundAmbient_group = soundEnvironment_group = nullptr;
		music_group = music_notification_group = soundNotification_group = nullptr;
		alcMakeContextCurrent(nullptr);
		alcDestroyContext(openal_context);
		openal_context = nullptr;
		alcCloseDevice(openal_device);
		openal_device = nullptr;
		return 0;
	}
	sound_group->volume = 1.0f;
	soundAmbient_group->volume = 1.0f;
	soundEnvironment_group->volume = 1.0f;
	music_group->volume = 1.0f;
	music_notification_group->volume = 1.0f;
	soundNotification_group->volume = 1.0f;

	memset(openal_sounds, 0, sizeof(openal_sounds));
	lower_freechannel = 0;
	upper_unfreechannel = 0;

	openal_mutex = SDL_CreateMutex();
	if (!openal_mutex) {
		printlog("[OpenAL]: failed to create the audio mutex: %s", SDL_GetError());
		free(sound_group);
		free(soundAmbient_group);
		free(soundEnvironment_group);
		free(music_group);
		free(music_notification_group);
		free(soundNotification_group);
		sound_group = soundAmbient_group = soundEnvironment_group = nullptr;
		music_group = music_notification_group = soundNotification_group = nullptr;
		alcMakeContextCurrent(nullptr);
		alcDestroyContext(openal_context);
		openal_context = nullptr;
		alcCloseDevice(openal_device);
		openal_device = nullptr;
		return 0;
	}
	openalSoundRunning.store(true);
	openal_soundthread = SDL_CreateThread(OPENAL_ThreadFunction, "openal", NULL);
	if (!openal_soundthread) {
		printlog("[OpenAL]: failed to create the audio thread: %s", SDL_GetError());
		openalSoundRunning.store(false);
		SDL_DestroyMutex(openal_mutex);
		openal_mutex = nullptr;
		free(sound_group);
		free(soundAmbient_group);
		free(soundEnvironment_group);
		free(music_group);
		free(music_notification_group);
		free(soundNotification_group);
		sound_group = soundAmbient_group = soundEnvironment_group = nullptr;
		music_group = music_notification_group = soundNotification_group = nullptr;
		alcMakeContextCurrent(nullptr);
		alcDestroyContext(openal_context);
		openal_context = nullptr;
		alcCloseDevice(openal_device);
		openal_device = nullptr;
		return 0;
	}

	openalInitialized = true;

#ifdef NINTENDO
	//TODO: Do we also want this on other platforms?
	// print source limit
	ALCint size = -1;
	alcGetIntegerv(openal_device, ALC_MONO_SOURCES, 1, &size);
	printlog("openAL: max mono sources: %d", size);
	size = -1;
	alcGetIntegerv(openal_device, ALC_STEREO_SOURCES, 1, &size);
	printlog("openAL: max stereo sources: %d", size);
#endif // NINTENDO

	return 1;
}

int closeOPENAL()
{
	if(!openalInitialized) return 0;

	openalSoundRunning.store(false);
	int i = 0;
	if (openal_soundthread) {
		SDL_WaitThread(openal_soundthread, &i);
		openal_soundthread = nullptr;
		if(i!=1) {
			printlog("Warning, unable to stop OpenAL thread\n");
		}
	}

	// stop all remaining sound
	for (int i=0; i<upper_unfreechannel; i++) {
		if(openal_sounds[i].active) {
			private_OPENAL_Channel_Stop(&openal_sounds[i]);
		}
	}
	lower_freechannel = 0;
	upper_unfreechannel = 0;
	music_channel = nullptr;
	music_channel2 = nullptr;
	music_resume = nullptr;

	if(openal_mutex) {
		SDL_DestroyMutex(openal_mutex);
		openal_mutex = NULL;
	}

	auto freeGroup = [](OPENAL_CHANNELGROUP*& group) {
		if (!group) {
			return;
		}
		free(group->sounds);
		free(group);
		group = nullptr;
	};
	freeGroup(sound_group);
	freeGroup(soundAmbient_group);
	freeGroup(soundEnvironment_group);
	freeGroup(music_group);
	freeGroup(music_notification_group);
	freeGroup(soundNotification_group);

	alcMakeContextCurrent(NULL);
	if (openal_context) {
		alcDestroyContext(openal_context);
		openal_context = NULL;
	}
	if (openal_device) {
		alcCloseDevice(openal_device);
		openal_device = NULL;
	}
	openalInitialized = false;

	return 1;
}


static int get_firstfreechannel()
{
	int i = lower_freechannel;
	while ((i<MAXSOUND) && (openal_sounds[i].active))
		i++;
	if (i<MAXSOUND) {
		return i;
	}
	// No free channels: prefer reclaiming an ordinary effect so music and map
	// ambience handles are not invalidated by a transient sound burst.
	for (i = MAXSOUND - 1; i >= 0; --i) {
		if (openal_sounds[i].active && openal_sounds[i].buffer
			&& !openal_sounds[i].buffer->stream
			&& !openal_sounds[i].buffer->persistent_channel) {
			break;
		}
	}
	if (i < 0) {
		i = MAXSOUND - 1;
	}

	private_OPENAL_Channel_Stop(&openal_sounds[i]);

	return i;
}

void setGlobalVolume(real_t master, real_t music, real_t gameplay, real_t ambient, real_t environment, real_t notification) {
    master = std::min(std::max(0.0, master), 1.0);
    music = std::min(std::max(0.0, music / 4.0), 1.0); // music volume cut in half because the music is loud...
    gameplay = std::min(std::max(0.0, gameplay), 1.0);
    ambient = std::min(std::max(0.0, ambient), 1.0);
    environment = std::min(std::max(0.0, environment), 1.0);
    notification = std::min(std::max(0.0, notification), 1.0);

	OPENAL_ChannelGroup_SetVolume(music_group, master * music);
	OPENAL_ChannelGroup_SetVolume(sound_group, master * gameplay);
	OPENAL_ChannelGroup_SetVolume(soundAmbient_group, master * ambient);
	OPENAL_ChannelGroup_SetVolume(soundEnvironment_group, master * environment);
    OPENAL_ChannelGroup_SetVolume(music_notification_group, master * notification);
	OPENAL_ChannelGroup_SetVolume(soundNotification_group, master * notification);
}

void sound_update(int player, int index, int numplayers)
{
	if (no_sound)
	{
		return;
	}
	if (!openal_device)
	{
		return;
	}

	FMOD_VECTOR position;

	auto& camera = cameras[index];
	if ( splitscreen )
	{
		camera = cameras[0];
	}

	position.x = -camera.y;
	position.y = -camera.z / 32;
	position.z = -camera.x;

	/*double cosroll = cos(0);
	double cosyaw = cos(camera.ang);
	double cospitch = cos(camera.vang);
	double sinroll = sin(0);
	double sinyaw = sin(camera.ang);
	double sinpitch = sin(camera.vang);

	double rx = sinroll*sinyaw - cosroll*sinpitch*cosyaw;
	double ry = sinroll*cosyaw + cosroll*sinpitch*sinyaw;
	double rz = cosroll*cospitch;*/

	float vector[6];
	vector[0] = 1 * sin(camera.ang);
	vector[1] = 0;
	vector[2] = 1 * cos(camera.ang);
	/*forward.x = rx;
	forward.y = ry;
	forward.z = rz;*/

	/*rx = sinroll*sinyaw - cosroll*cospitch*cosyaw;
	ry = sinroll*cosyaw + cosroll*cospitch*sinyaw;
	rz = cosroll*sinpitch;*/

	vector[3] = 0;
	vector[4] = 1;
	vector[5] = 0;
	/*up.x = rx;
	up.y = ry;
	up.z = rz;*/

	if (openal_mutex) SDL_LockMutex(openal_mutex);
	alListenerfv(AL_POSITION, (float*)&position);
	alListenerfv(AL_ORIENTATION, vector);
	if (openal_mutex) SDL_UnlockMutex(openal_mutex);
	//FMOD_System_Set3DListenerAttributes(fmod_system, 0, &position, 0, &forward, &up);

	//Fade in the currently playing music.
	if (player == 0)
	{
		if (music_channel)
		{
			ALboolean playing = AL_FALSE;
			OPENAL_Channel_IsPlaying(music_channel, &playing);
			if (playing == AL_TRUE)
			{
				float volume = OPENAL_Channel_GetVolume(music_channel);
				if (volume < 1.0f)
				{
					volume += fadein_increment * 2;
					if (volume > 1.0f)
					{
						volume = 1.0f;
					}
					OPENAL_Channel_SetVolume(music_channel, volume);
				}
			}
		}
		//The following makes crossfading possible. Fade out the last playing music. //TODO: Support for saving music so that it can be resumed (for stuff interrupting like combat music).
		if (music_channel2)
		{
			ALboolean playing = AL_FALSE;
			OPENAL_Channel_IsPlaying(music_channel2, &playing);
			if (playing == AL_TRUE)
			{
				float volume = OPENAL_Channel_GetVolume(music_channel2);
				if (volume > 0.0f)
				{
					volume -= fadeout_increment * 2;
					if (volume < 0.0f)
					{
						volume = 0.0f;
					}
					OPENAL_Channel_SetVolume(music_channel2, volume);
				}
				else
				{
					OPENAL_Channel_Pause(music_channel2);
				}
			}
		}
#ifndef EDITOR
		updateMapAmbience();
#endif
	}
}

void OPENAL_Channel_SetVolume(OPENAL_SOUND *channel, float f) {
	if (!channel || !openal_mutex) return;
	SDL_LockMutex(openal_mutex);
	if (channel->active) {
		channel->volume = f;
		if(channel->group)
			f *= channel->group->volume;
		alSourcef(channel->id, AL_GAIN, f);
	}
	SDL_UnlockMutex(openal_mutex);
}

float OPENAL_Channel_GetVolume(OPENAL_SOUND *channel) {
	if (!channel || !openal_mutex) return 0.f;
	SDL_LockMutex(openal_mutex);
	const float volume = channel->active ? channel->volume : 0.f;
	SDL_UnlockMutex(openal_mutex);
	return volume;
}

void OPENAL_ChannelGroup_Stop(OPENAL_CHANNELGROUP* group) {
	if (!group || !openal_mutex) return;
	SDL_LockMutex(openal_mutex);
	while (group->num > 0) {
		OPENAL_SOUND* channel = group->sounds[group->num - 1];
		if (channel && channel->active) {
			const int index = channel->indice;
			private_OPENAL_Channel_Stop(channel);
			if (lower_freechannel > index) {
				lower_freechannel = index;
			}
		} else {
			--group->num;
		}
	}
	SDL_UnlockMutex(openal_mutex);
}

void OPENAL_ChannelGroup_SetVolume(OPENAL_CHANNELGROUP* group, float f) {
	if (!group || !openal_mutex) return;
	SDL_LockMutex(openal_mutex);
	group->volume = f;
	for (int i = 0; i< group->num; i++) {
		if (group->sounds[i] && group->sounds[i]->active)
			alSourcef( group->sounds[i]->id, AL_GAIN, f*group->sounds[i]->volume );
	}
	SDL_UnlockMutex(openal_mutex);
}

void OPENAL_Channel_SetChannelGroup(OPENAL_SOUND *channel, OPENAL_CHANNELGROUP *group) {
	if (!channel || !channel->active || !group || !openal_mutex) return;
	SDL_LockMutex(openal_mutex);
	if (channel->group == group) {
		alSourcef(channel->id, AL_GAIN, channel->volume * group->volume);
		SDL_UnlockMutex(openal_mutex);
		return;
	}
	if (channel->group) {
		OPENAL_RemoveChannelGroup(channel, channel->group);
	}
	if(group->num==group->cap) {
		const int newCapacity = group->cap + 8;
		OPENAL_SOUND** grown = (OPENAL_SOUND**)realloc(
			group->sounds, newCapacity * sizeof(OPENAL_SOUND*));
		if (!grown) {
			SDL_UnlockMutex(openal_mutex);
			return;
		}
		group->sounds = grown;
		group->cap = newCapacity;
	}
	alSourcef(channel->id, AL_GAIN, channel->volume * group->volume);
	group->sounds[group->num++] = channel;
	channel->group = group;
	SDL_UnlockMutex(openal_mutex);
}

void OPENAL_RemoveChannelGroup(OPENAL_SOUND *channel, OPENAL_CHANNELGROUP *group) {
	if (!channel || !group) return;
	int i = 0;
	while ((i<group->num) && (channel!=group->sounds[i]))
		i++;
	if(i==group->num)
		return;
	memmove(group->sounds+i, group->sounds+i+1, sizeof(OPENAL_SOUND*)*(group->num-(i+1)));
	group->num--;
	if (group->sounds) {
		group->sounds[group->num] = nullptr;
	}
	channel->group = nullptr;
}

static size_t openal_file_oggread(void* ptr, size_t size, size_t nmemb, void* datasource) {
	File* file = (File*)datasource;
	if (!file || size == 0) return 0;
	return file->read(ptr, size, nmemb);
}

static int openal_file_oggseek(void* datasource, ogg_int64_t offset, int whence) {
	File* file = (File*)datasource;
	if (!file) return -1;
	const size_t fileSize = file->size();
	const long int positionBeforeSeek = file->tell();
	bool requestedEndOfFile = false;
	File::SeekMode mode = File::SeekMode::SET;
	switch (whence) {
	case SEEK_CUR:
		mode = File::SeekMode::ADD;
		if (positionBeforeSeek >= 0 && offset >= 0
			&& static_cast<size_t>(positionBeforeSeek) <= fileSize
			&& static_cast<uint64_t>(offset)
				== fileSize - static_cast<size_t>(positionBeforeSeek)) {
			requestedEndOfFile = true;
		}
		break;
	case SEEK_END:
		mode = File::SeekMode::SETEND;
		requestedEndOfFile = offset == 0;
		break;
	case SEEK_SET:
		mode = File::SeekMode::SET;
		requestedEndOfFile = offset >= 0
			&& static_cast<uint64_t>(offset) == fileSize;
		break;
	default:
		return -1;
	}
	if (offset < std::numeric_limits<ptrdiff_t>::min()
		|| offset > std::numeric_limits<ptrdiff_t>::max()) {
		return -1;
	}
	const int result = file->seek(static_cast<ptrdiff_t>(offset), mode);
	// FilePC historically reports EOF as a seek error even when it moved to the
	// requested end position. Vorbis expects stdio-style success for that seek.
	if (result != 0 && requestedEndOfFile && file->tell() >= 0
		&& static_cast<size_t>(file->tell()) == fileSize) {
		return 0;
	}
	return result;
}

static int openal_file_oggclose(void* datasource) {
	return 0;
}

static long int openal_file_oggtell(void* datasource) {
	File* file = (File*)datasource;
	return file ? file->tell() : -1;
}

static bool openalHasExtension(const char* name, const char* extension) {
	if (!name || !extension) return false;
	const size_t nameLength = strlen(name);
	const size_t extensionLength = strlen(extension);
	if (nameLength < extensionLength) return false;
	for (size_t i = 0; i < extensionLength; ++i) {
		const unsigned char lhs = static_cast<unsigned char>(
			name[nameLength - extensionLength + i]);
		const unsigned char rhs = static_cast<unsigned char>(extension[i]);
		if (std::tolower(lhs) != std::tolower(rhs)) return false;
	}
	return true;
}

static bool openalUploadPcm(OPENAL_BUFFER* buffer, const int channels,
	const void* data, const size_t size, const int frequency) {
	if (!buffer || !data || size == 0
		|| size > static_cast<size_t>(std::numeric_limits<ALsizei>::max())
		|| (channels != 1 && channels != 2) || frequency <= 0) {
		return false;
	}
	if (!openal_context) {
		return false;
	}
	if (openal_mutex) SDL_LockMutex(openal_mutex);
	alGetError();
	alGenBuffers(1, &buffer->id);
	if (alGetError() != AL_NO_ERROR || buffer->id == 0) {
		buffer->id = 0;
		if (openal_mutex) SDL_UnlockMutex(openal_mutex);
		return false;
	}
	alBufferData(buffer->id,
		channels == 1 ? AL_FORMAT_MONO16 : AL_FORMAT_STEREO16,
		data, static_cast<ALsizei>(size), frequency);
	if (alGetError() != AL_NO_ERROR) {
		alDeleteBuffers(1, &buffer->id);
		buffer->id = 0;
		if (openal_mutex) SDL_UnlockMutex(openal_mutex);
		return false;
	}
	if (openal_mutex) SDL_UnlockMutex(openal_mutex);
	return true;
}

static bool openalLoadWav(File* file, const char* name, const bool b3D,
	OPENAL_BUFFER* buffer) {
	const size_t fileSize = file ? file->size() : 0;
	if (fileSize == 0 || fileSize > static_cast<size_t>(std::numeric_limits<int>::max())) {
		return false;
	}
	std::vector<Uint8> encoded(fileSize);
	if (file->read(encoded.data(), sizeof(Uint8), fileSize) != fileSize) {
		return false;
	}
	SDL_RWops* rw = SDL_RWFromConstMem(encoded.data(), static_cast<int>(fileSize));
	if (!rw) {
		return false;
	}
	SDL_AudioSpec sourceSpec{};
	Uint8* sourceData = nullptr;
	Uint32 sourceLength = 0;
	if (!SDL_LoadWAV_RW(rw, 1, &sourceSpec, &sourceData, &sourceLength)) {
		printlog("[OpenAL]: invalid WAV file '%s': %s", name, SDL_GetError());
		return false;
	}

	const Uint8 targetChannels = b3D || sourceSpec.channels == 1 ? 1 : 2;
	SDL_AudioCVT converter{};
	const int conversion = SDL_BuildAudioCVT(&converter,
		sourceSpec.format, sourceSpec.channels, sourceSpec.freq,
		AUDIO_S16SYS, targetChannels, sourceSpec.freq);
	if (conversion < 0) {
		printlog("[OpenAL]: cannot convert WAV file '%s': %s", name, SDL_GetError());
		SDL_FreeWAV(sourceData);
		return false;
	}

	const Uint8* pcmData = sourceData;
	size_t pcmLength = sourceLength;
	Uint8* convertedData = nullptr;
	if (conversion > 0) {
		if (sourceLength > static_cast<Uint32>(
			std::numeric_limits<int>::max() / std::max(1, converter.len_mult))) {
			SDL_FreeWAV(sourceData);
			return false;
		}
		converter.len = static_cast<int>(sourceLength);
		converter.buf = (Uint8*)SDL_malloc(
			static_cast<size_t>(sourceLength) * converter.len_mult);
		if (!converter.buf) {
			SDL_FreeWAV(sourceData);
			return false;
		}
		memcpy(converter.buf, sourceData, sourceLength);
		if (SDL_ConvertAudio(&converter) < 0) {
			printlog("[OpenAL]: cannot convert WAV file '%s': %s", name, SDL_GetError());
			SDL_free(converter.buf);
			SDL_FreeWAV(sourceData);
			return false;
		}
		convertedData = converter.buf;
		pcmData = convertedData;
		pcmLength = static_cast<size_t>(converter.len_cvt);
	}

	const bool loaded = openalUploadPcm(
		buffer, targetChannels, pcmData, pcmLength, sourceSpec.freq);
	SDL_free(convertedData);
	SDL_FreeWAV(sourceData);
	return loaded;
}

static bool openalLoadOgg(File* file, const char* name, const bool b3D,
	OPENAL_BUFFER* buffer) {
	if (!file) return false;
	ov_callbacks oggCallbacks = {
		openal_file_oggread, openal_file_oggseek,
		openal_file_oggclose, openal_file_oggtell
	};
	OggVorbis_File oggFile{};
	if (ov_open_callbacks(file, &oggFile, nullptr, 0, oggCallbacks) != 0) {
		printlog("[OpenAL]: invalid OGG file '%s'.", name);
		return false;
	}
	vorbis_info* info = ov_info(&oggFile, -1);
	const ogg_int64_t frameCount = ov_pcm_total(&oggFile, -1);
	if (!info || (info->channels != 1 && info->channels != 2)
		|| info->rate <= 0 || frameCount <= 0
		|| static_cast<uint64_t>(frameCount)
			> std::numeric_limits<size_t>::max()
				/ (static_cast<size_t>(info->channels) * sizeof(int16_t))) {
		printlog("[OpenAL]: unsupported OGG format in '%s'.", name);
		ov_clear(&oggFile);
		return false;
	}

	const size_t sampleCount = static_cast<size_t>(frameCount)
		* static_cast<size_t>(info->channels);
	const size_t capacity = sampleCount * sizeof(int16_t);
	std::vector<int16_t> decoded(sampleCount);
	char* decodedBytes = reinterpret_cast<char*>(decoded.data());
	size_t decodedLength = 0;
	bool decodeFailed = false;
	while (decodedLength < capacity) {
		int bitStream = 0;
		const int request = static_cast<int>(std::min(
			capacity - decodedLength,
			static_cast<size_t>(std::numeric_limits<int>::max())));
#ifdef USE_TREMOR
		const long bytes = ov_read(
			&oggFile, decodedBytes + decodedLength, request, &bitStream);
#else
		const long bytes = ov_read(
			&oggFile, decodedBytes + decodedLength, request, 0, 2, 1, &bitStream);
#endif
		if (bytes == 0) break;
		if (bytes < 0) {
			decodeFailed = true;
			break;
		}
		decodedLength += static_cast<size_t>(bytes);
	}
	const int sourceChannels = info->channels;
	const int frequency = info->rate;
	ov_clear(&oggFile);
	if (decodeFailed || decodedLength == 0
		|| decodedLength % (sourceChannels * sizeof(int16_t)) != 0) {
		printlog("[OpenAL]: failed decoding OGG file '%s'.", name);
		return false;
	}

	if (b3D && sourceChannels == 2) {
		const size_t stereoFrames = decodedLength / (2 * sizeof(int16_t));
		std::vector<int16_t> mono(stereoFrames);
		const int16_t* stereo = decoded.data();
		for (size_t i = 0; i < stereoFrames; ++i) {
			const int mixed = static_cast<int>(stereo[i * 2])
				+ static_cast<int>(stereo[i * 2 + 1]);
			mono[i] = static_cast<int16_t>(mixed / 2);
		}
		return openalUploadPcm(
			buffer, 1, mono.data(), mono.size() * sizeof(int16_t), frequency);
	}
	return openalUploadPcm(
		buffer, sourceChannels, decoded.data(), decodedLength, frequency);
}

int OPENAL_CreateSound(const char* name, bool b3D, OPENAL_BUFFER **buffer) {
	if (!buffer || !name) {
		return 0;
	}
	*buffer = (OPENAL_BUFFER*)calloc(1, sizeof(OPENAL_BUFFER));
	if (!*buffer) {
		return 0;
	}
	snprintf((*buffer)->oggfile, sizeof((*buffer)->oggfile), "%s", name);
	(*buffer)->stream = false;
	(*buffer)->loop = false;
	(*buffer)->persistent_channel = false;
	if (strlen(name) >= sizeof((*buffer)->oggfile)) {
		printlog("[OpenAL]: audio path is too long: '%s'.", name);
		free(*buffer);
		*buffer = nullptr;
		return 0;
	}
	File *f = openDataFile(name, "rb");
	if(!f) {
		printlog("[OpenAL]: error opening sound '%s'.", name);
		free(*buffer);
		*buffer = nullptr;
		return 0;
	}
	const bool loaded = openalHasExtension(name, ".wav")
		? openalLoadWav(f, name, b3D, *buffer)
		: openalLoadOgg(f, name, b3D, *buffer);
	FileIO::close(f);
	if (!loaded) {
		free(*buffer);
		*buffer = nullptr;
		return 0;
	}
	return 1;
}

int OPENAL_CreateStreamSound(const char* name, OPENAL_BUFFER **buffer) {
	if (!buffer || !name) {
		return 0;
	}
	// OpenAL's streaming decoder is Vorbis-based. WAV remains fully supported,
	// but is decoded into a regular buffer when streaming was requested.
	if (openalHasExtension(name, ".wav")) {
		const int result = OPENAL_CreateSound(name, false, buffer);
		if (result && *buffer) {
			(*buffer)->persistent_channel = true;
		}
		return result;
	}
	*buffer = (OPENAL_BUFFER*)calloc(1, sizeof(OPENAL_BUFFER));
	if (!*buffer) {
		return 0;
	}
	(*buffer)->stream = true;
	(*buffer)->loop = false;
	(*buffer)->persistent_channel = true;
	if (strlen(name) >= sizeof((*buffer)->oggfile)) {
		printlog("[OpenAL]: audio path is too long: '%s'.", name);
		free(*buffer);
		*buffer = nullptr;
		return 0;
	}
	snprintf((*buffer)->oggfile, sizeof((*buffer)->oggfile), "%s", name);
	return 1;
}

OPENAL_SOUND* OPENAL_CreateChannel(OPENAL_BUFFER* buffer) {
	if (!buffer || !openal_context || !openal_mutex) {
		return nullptr;
	}
	SDL_LockMutex(openal_mutex);

	int i = get_firstfreechannel();

	if(upper_unfreechannel < (i+1))
		upper_unfreechannel = i+1;
	lower_freechannel = i+1;

	OPENAL_SOUND *channel = &openal_sounds[i];
	memset(channel, 0, sizeof(*channel));
	alGetError();
	alGenSources(1,&channel->id);
	if (alGetError() != AL_NO_ERROR || channel->id == 0) {
		channel->active = false;
		if (lower_freechannel > i) lower_freechannel = i;
		SDL_UnlockMutex(openal_mutex);
		return nullptr;
	}
	channel->volume = 1.0f;
	channel->group = NULL;
	channel->active = true;
	channel->loop = buffer->loop;
	channel->buffer = buffer;
	channel->stream_active = false;
	channel->stream_open = false;
	channel->indice = i;

	if(buffer->stream) {
		if (!openal_oggopen(channel, buffer->oggfile)) {
			private_OPENAL_Channel_Stop(channel);
			if (lower_freechannel > i) lower_freechannel = i;
			SDL_UnlockMutex(openal_mutex);
			return nullptr;
		}
	} else {
		alSourcei(channel->id, AL_BUFFER, buffer->id);
		alSourcei(channel->id, AL_LOOPING, buffer->loop ? AL_TRUE : AL_FALSE);
	}
	// default to 2D...
	alSourcei(channel->id,AL_SOURCE_RELATIVE, AL_TRUE);
	alSource3f(channel->id, AL_POSITION, 0, 0, 0);

	SDL_UnlockMutex(openal_mutex);
	return channel;
}

void OPENAL_Channel_IsPlaying(void* channel, ALboolean *playing) {
	if (!playing) return;
	if (!channel || !openal_mutex) {
		*playing = AL_FALSE;
		return;
	}
	SDL_LockMutex(openal_mutex);
	OPENAL_SOUND* openalChannel = (OPENAL_SOUND*)channel;
	if (!openalChannel->active) {
		*playing = AL_FALSE;
	} else {
		ALint state = AL_STOPPED;
		alGetSourcei(openalChannel->id, AL_SOURCE_STATE, &state);
		*playing = state == AL_PLAYING ? AL_TRUE : AL_FALSE;
	}
	SDL_UnlockMutex(openal_mutex);
}

void OPENAL_Channel_Stop(void* chan) {
	OPENAL_SOUND* channel = (OPENAL_SOUND*)chan;
	if(channel==NULL || !openal_mutex) {
		return;
	}
	SDL_LockMutex(openal_mutex);

	if(!channel->active) {
		SDL_UnlockMutex(openal_mutex);
		return;
	}

	int i = channel->indice;
	private_OPENAL_Channel_Stop(channel);
	if (lower_freechannel > i)
		lower_freechannel = i;


	SDL_UnlockMutex(openal_mutex);
}

void OPENAL_Channel_Set3DAttributes(OPENAL_SOUND* channel, float x, float y, float z) {
	if (!channel || !openal_mutex) return;
	SDL_LockMutex(openal_mutex);
	if (channel->active) {
		alSourcei(channel->id,AL_SOURCE_RELATIVE, AL_FALSE);
		alSource3f(channel->id, AL_POSITION, x, y, z);
		alSourcef(channel->id, AL_REFERENCE_DISTANCE, 1.f);	// hardcoding FMOD_System_Set3DSettings(fmod_system, 1.0, 2.0, 1.0);
		alSourcef(channel->id, AL_MAX_DISTANCE, 10.f);		// but this are simply OpenAL default (the 2.0f is used for Dopler only)
	}
	SDL_UnlockMutex(openal_mutex);
}

void OPENAL_Channel_Play(OPENAL_SOUND* channel) {
	if (!channel || !channel->active || !openal_mutex) return;
	SDL_LockMutex(openal_mutex);

	ALint state;
	alGetSourcei( channel->id, AL_SOURCE_STATE, &state );
	if(state != AL_PLAYING && state != AL_PAUSED) {
		if(channel->buffer->stream) {
			int processed = 0;
			int num_buffers = 4;
			int i;
			ALuint trash[256];

			alGetSourcei(channel->id, AL_BUFFERS_PROCESSED, &processed);
			alSourceUnqueueBuffers(channel->id, processed, trash);

			for(i=0; i<4; i++) {
				if(!openal_streamread(channel, channel->streambuff[i])) {
					num_buffers = i;
					break;
				}
			}

			if (num_buffers > 0) {
				alSourceQueueBuffers(channel->id, num_buffers, channel->streambuff);
				channel->stream_active = true;
			} else {
				channel->stream_active = false;
			}
		}
	}
	if (!channel->buffer->stream || channel->stream_active) {
		alSourcePlay(channel->id);
	}

	SDL_UnlockMutex(openal_mutex);
}

void OPENAL_Channel_Pause(OPENAL_SOUND* channel) {
	if (!channel || !openal_mutex) return;
	SDL_LockMutex(openal_mutex);
	if (channel->active) alSourcePause(channel->id);
	SDL_UnlockMutex(openal_mutex);
}

void OPENAL_GetBuffer(OPENAL_SOUND* channel, OPENAL_BUFFER** buffer) {
	if (!buffer) return;
	if (!channel || !openal_mutex) {
		*buffer = nullptr;
		return;
	}
	SDL_LockMutex(openal_mutex);
	*buffer = channel->active ? channel->buffer : nullptr;
	SDL_UnlockMutex(openal_mutex);
}

void OPENAL_SetLoop(OPENAL_SOUND* channel, ALboolean looping) {
	if (!channel || !openal_mutex) return;
	SDL_LockMutex(openal_mutex);
	if (channel->active && channel->buffer) {
		channel->loop = looping;
		if(!channel->buffer->stream)
			alSourcei(channel->id, AL_LOOPING, looping);
	}
	SDL_UnlockMutex(openal_mutex);
}

void OPENAL_Channel_GetPosition(OPENAL_SOUND* channel, unsigned int *position) {
	if (!position) return;
	if (!channel || !openal_mutex) {
		*position = 0;
		return;
	}
	SDL_LockMutex(openal_mutex);
	if (channel->active) {
		alGetSourcei(channel->id, AL_BYTE_OFFSET, (GLint*)position);
	} else {
		*position = 0;
	}
	SDL_UnlockMutex(openal_mutex);
}

void OPENAL_Sound_GetLength(OPENAL_BUFFER* buffer, unsigned int *length) {
	if(!length) return;
	*length = 0;
	if(!buffer || buffer->stream || !openal_context) return;
	if (openal_mutex) SDL_LockMutex(openal_mutex);
	alGetBufferi(buffer->id, AL_SIZE, (GLint*)length);
	if (openal_mutex) SDL_UnlockMutex(openal_mutex);
}

void OPENAL_Sound_SetDefaultLoop(OPENAL_BUFFER* buffer, ALboolean looping) {
	if (!buffer) return;
	buffer->loop = looping == AL_TRUE;
}

void OPENAL_Sound_Release(OPENAL_BUFFER* buffer) {
	if(!buffer) return;
	if (openal_mutex) {
		SDL_LockMutex(openal_mutex);
		for (int i = 0; i < upper_unfreechannel; ++i) {
			if (openal_sounds[i].active && openal_sounds[i].buffer == buffer) {
				private_OPENAL_Channel_Stop(&openal_sounds[i]);
				if (lower_freechannel > i) lower_freechannel = i;
			}
		}
		if(!buffer->stream && openal_context)
			alDeleteBuffers( 1, &buffer->id );
		SDL_UnlockMutex(openal_mutex);
	} else if(!buffer->stream && openal_context) {
		alDeleteBuffers( 1, &buffer->id );
	}
	free(buffer);
}

#endif

#ifdef USE_OPENAL
extern const std::vector<std::string> themeMusic;

static bool reloadOpenALMusicBuffer(
    const std::string& filename,
    OPENAL_BUFFER*& buffer
)
{
    if ( buffer )
    {
        OPENAL_Sound_Release(buffer);
        buffer = nullptr;
    }

    const int result = musicPreload
        ? OPENAL_CreateSound(filename.c_str(), false, &buffer)
        : OPENAL_CreateStreamSound(filename.c_str(), &buffer);
    if (result != 0 && buffer)
    {
        buffer->persistent_channel = true;
    }
    if ( result == 0 )
    {
        printlog(
            "[PhysFS]: ERROR: Failed reloading music file \"%s\".",
            filename.c_str()
        );
        return false;
    }
    return true;
}

static bool reloadOpenALMusicArray(
    const uint32_t count,
    const char* filenameTemplate,
    OPENAL_BUFFER** musicArray,
    const bool reloadAll
)
{
    if ( !musicArray )
    {
        return false;
    }
    bool success = true;
    for ( uint32_t index = 0; index < count; ++index )
    {
        snprintf(tempstr, 1000, filenameTemplate, index);
        const char* realDirectory = PHYSFS_getRealDir(tempstr);
        if ( !realDirectory )
        {
            continue;
        }
        std::string musicPath = realDirectory;
        if ( musicPath == "./" && !reloadAll )
        {
            continue;
        }
        musicPath.append(PHYSFS_getDirSeparator()).append(tempstr);
        printlog("[PhysFS]: Loading music file %s...", tempstr);
        success = reloadOpenALMusicBuffer(musicPath, musicArray[index])
            && success;
    }
    return success;
}

static void physfsReloadMusicOpenAL(
    bool& introMusicChanged,
    const bool reloadAll
)
{
    introMusicChanged = false;
    size_t index = 0;
    for ( const std::string& filename : themeMusic )
    {
        OPENAL_BUFFER** destination = nullptr;
        switch ( index )
        {
            case 0: destination = &introductionmusic; break;
            case 1: destination = &intermissionmusic; break;
            case 2: destination = &minetownmusic; break;
            case 3: destination = &splashmusic; break;
            case 4: destination = &librarymusic; break;
            case 5: destination = &shopmusic; break;
            case 6: destination = &herxmusic; break;
            case 7: destination = &templemusic; break;
            case 8: destination = &endgamemusic; break;
            case 9: destination = &escapemusic; break;
            case 10: destination = &devilmusic; break;
            case 11: destination = &sanctummusic; break;
            case 12: destination = &gnomishminesmusic; break;
            case 13: destination = &greatcastlemusic; break;
            case 14: destination = &sokobanmusic; break;
            case 15: destination = &caveslairmusic; break;
            case 16: destination = &bramscastlemusic; break;
            case 17: destination = &hamletmusic; break;
            case 18: destination = &tutorialmusic; break;
            case 19: destination = &gameovermusic; break;
            case 20: destination = &introstorymusic; break;
            default: break;
        }

        const char* realDirectory = PHYSFS_getRealDir(filename.c_str());
        if ( destination && realDirectory )
        {
            std::string musicPath = realDirectory;
            if ( musicPath != "./" || reloadAll )
            {
                musicPath += PHYSFS_getDirSeparator() + filename;
                printlog("[PhysFS]: Loading music file %s...", filename.c_str());
                reloadOpenALMusicBuffer(musicPath, *destination);
            }
        }
        ++index;
    }

    reloadOpenALMusicArray(NUMMINESMUSIC, "music/mines%02d.ogg", minesmusic, reloadAll);
    reloadOpenALMusicArray(NUMSWAMPMUSIC, "music/swamp%02d.ogg", swampmusic, reloadAll);
    reloadOpenALMusicArray(NUMLABYRINTHMUSIC, "music/labyrinth%02d.ogg", labyrinthmusic, reloadAll);
    reloadOpenALMusicArray(NUMRUINSMUSIC, "music/ruins%02d.ogg", ruinsmusic, reloadAll);
    reloadOpenALMusicArray(NUMUNDERWORLDMUSIC, "music/underworld%02d.ogg", underworldmusic, reloadAll);
    reloadOpenALMusicArray(NUMHELLMUSIC, "music/hell%02d.ogg", hellmusic, reloadAll);
    reloadOpenALMusicArray(NUMMINOTAURMUSIC, "music/minotaur%02d.ogg", minotaurmusic, reloadAll);
    reloadOpenALMusicArray(NUMCAVESMUSIC, "music/caves%02d.ogg", cavesmusic, reloadAll);
    reloadOpenALMusicArray(NUMCITADELMUSIC, "music/citadel%02d.ogg", citadelmusic, reloadAll);

    for ( int intro = 0; intro < NUMINTROMUSIC; ++intro )
    {
        if ( intro == 0 )
        {
            strcpy(tempstr, "music/intro.ogg");
        }
        else
        {
            snprintf(tempstr, 1000, "music/intro%02d.ogg", intro);
        }
        const char* realDirectory = PHYSFS_getRealDir(tempstr);
        if ( !realDirectory )
        {
            continue;
        }
        std::string musicPath = realDirectory;
        if ( musicPath == "./" && !reloadAll )
        {
            continue;
        }
        musicPath.append(PHYSFS_getDirSeparator()).append(tempstr);
        printlog("[PhysFS]: Loading music file %s...", tempstr);
        if ( reloadOpenALMusicBuffer(musicPath, intromusic[intro]) )
        {
            introMusicChanged = true;
        }
    }
}
#endif

bool physfsSearchMusicToUpdate_helper_findModifiedMusic(uint32_t numMusic, const char* filenameTemplate)
{
	for ( int c = 0; c < numMusic; c++ )
	{
		snprintf(tempstr, 1000, filenameTemplate, c);
		if ( PHYSFS_getRealDir(tempstr) != nullptr )
		{
			std::string musicDir = PHYSFS_getRealDir(tempstr);
			if ( musicDir.compare("./") != 0 )
			{
				printlog("[PhysFS]: Found modified music in music/ directory, reloading music files...");
				return true;
			}
		}
	}

	return false;
}

const std::vector<std::string> themeMusic = {
	"music/introduction.ogg",
	"music/intermission.ogg",
	"music/minetown.ogg",
	"music/splash.ogg",
	"music/library.ogg",
	"music/shop.ogg",
	"music/herxboss.ogg",
	"music/temple.ogg",
	"music/endgame.ogg",
	"music/escape.ogg",
	"music/devil.ogg",
	"music/sanctum.ogg",
	"music/gnomishmines.ogg",
	"music/greatcastle.ogg",
	"music/sokoban.ogg",
	"music/caveslair.ogg",
	"music/bramscastle.ogg",
	"music/hamlet.ogg",
	"music/tutorial.ogg",
	"sound/Death.ogg",
	"sound/ui/StoryMusicV3.ogg",
	"sound/ensemble/ensemble1_drumV1.ogg",
	"sound/ensemble/ensemble1_fluteV1.ogg",
	"sound/ensemble/ensemble1_hornV1.ogg",
	"sound/ensemble/ensemble1_luteV1.ogg",
	"sound/ensemble/ensemble1_lyreV1.ogg",
	"sound/ensemble/ensemble1_tamboV1.ogg",
	"sound/ensemble/ensemble1_BEB_tier1_V1.ogg",
	"sound/ensemble/ensemble1_BEB_tier2_V1.ogg",
	"sound/ensemble/ensemble1_drum_combatV1.ogg",
	"sound/ensemble/ensemble1_flute_combatV1.ogg",
	"sound/ensemble/ensemble1_horn_combatV1.ogg",
	"sound/ensemble/ensemble1_lute_combatV1.ogg",
	"sound/ensemble/ensemble1_lyre_combatV1.ogg",
	"sound/ensemble/ensemble1_tambo_combatV1.ogg",
	"sound/ensemble/ensemble1_BEB_tier1_combatV1.ogg",
	"sound/ensemble/ensemble1_BEB_tier2_combatV1.ogg",
	/*"sound/ensemble/Trans1/ensemble1_drum_Trans1_120_4-4_V1.ogg",
	"sound/ensemble/Trans1/ensemble1_flute_Trans1_120_4-4_V1.ogg",
	"sound/ensemble/Trans1/ensemble1_horn_Trans1_120_4-4_V1.ogg",
	"sound/ensemble/Trans1/ensemble1_lute_Trans1_120_4-4_V1.ogg",
	"sound/ensemble/Trans1/ensemble1_lyre_Trans1_120_4-4_V1.ogg",
	"sound/ensemble/Trans1/ensemble1_tambo_Trans1_120_4-4_V1.ogg",
	"sound/ensemble/Trans1/ensemble1_tambo_Trans1_120_4-4_V1.ogg",
	"sound/ensemble/Trans1/ensemble1_tambo_Trans1_120_4-4_V1.ogg",
	"sound/ensemble/Trans2/ensemble1_drum_Trans2_120_4-4_V1.ogg",
	"sound/ensemble/Trans2/ensemble1_flute_Trans2_120_4-4_V1.ogg",
	"sound/ensemble/Trans2/ensemble1_horn_Trans2_120_4-4_V1.ogg",
	"sound/ensemble/Trans2/ensemble1_lute_Trans2_120_4-4_V1.ogg",
	"sound/ensemble/Trans2/ensemble1_lyre_Trans2_120_4-4_V1.ogg",
	"sound/ensemble/Trans2/ensemble1_tambo_Trans2_120_4-4_V1.ogg",
	"sound/ensemble/Trans2/ensemble1_tambo_Trans2_120_4-4_V1.ogg",
	"sound/ensemble/Trans2/ensemble1_tambo_Trans2_120_4-4_V1.ogg",
	"sound/ensemble/Trans3/ensemble1_drum_Trans3_120_4-4_V1.ogg",
	"sound/ensemble/Trans3/ensemble1_flute_Trans3_120_4-4_V1.ogg",
	"sound/ensemble/Trans3/ensemble1_horn_Trans3_120_4-4_V1.ogg",
	"sound/ensemble/Trans3/ensemble1_lute_Trans3_120_4-4_V1.ogg",
	"sound/ensemble/Trans3/ensemble1_lyre_Trans3_120_4-4_V1.ogg",
	"sound/ensemble/Trans3/ensemble1_tambo_Trans3_120_4-4_V1.ogg",
	"sound/ensemble/Trans3/ensemble1_tambo_Trans3_120_4-4_V1.ogg",
	"sound/ensemble/Trans3/ensemble1_tambo_Trans3_120_4-4_V1.ogg",*/
	"sound/ensemble/CombatEnd1/ensemble1_drum_combat_End1_90_7-8.ogg",
	"sound/ensemble/CombatEnd1/ensemble1_flute_combat_End1_90_7-8.ogg",
	"sound/ensemble/CombatEnd1/ensemble1_horn_combat_End1_90_7-8.ogg",
	"sound/ensemble/CombatEnd1/ensemble1_lute_combat_End1_90_7-8.ogg",
	"sound/ensemble/CombatEnd1/ensemble1_lyre_combat_End1_90_7-8.ogg",
	"sound/ensemble/CombatEnd1/ensemble1_tambo_combat_End1_90_7-8.ogg",
	"sound/ensemble/CombatEnd1/ensemble1_BEB_tier1_combat_End1_90_7-8.ogg",
	"sound/ensemble/CombatEnd1/ensemble1_BEB_tier2_combat_End1_90_7-8.ogg",
	"sound/ensemble/CombatEnd2/ensemble1_drum_combat_End2_90_7-8.ogg",
	"sound/ensemble/CombatEnd2/ensemble1_flute_combat_End2_90_7-8.ogg",
	"sound/ensemble/CombatEnd2/ensemble1_horn_combat_End2_90_7-8.ogg",
	"sound/ensemble/CombatEnd2/ensemble1_lute_combat_End2_90_7-8.ogg",
	"sound/ensemble/CombatEnd2/ensemble1_lyre_combat_End2_90_7-8.ogg",
	"sound/ensemble/CombatEnd2/ensemble1_tambo_combat_End2_90_7-8.ogg",
	"sound/ensemble/CombatEnd2/ensemble1_BEB_tier1_combat_End2_90_7-8.ogg",
	"sound/ensemble/CombatEnd2/ensemble1_BEB_tier2_combat_End2_90_7-8.ogg",
	/*"sound/ensemble/CombatEnd3/ensemble1_drum_combat_End3_90_7-8.ogg",
	"sound/ensemble/CombatEnd3/ensemble1_flute_combat_End3_90_7-8.ogg",
	"sound/ensemble/CombatEnd3/ensemble1_horn_combat_End3_90_7-8.ogg",
	"sound/ensemble/CombatEnd3/ensemble1_lute_combat_End3_90_7-8.ogg",
	"sound/ensemble/CombatEnd3/ensemble1_lyre_combat_End3_90_7-8.ogg",
	"sound/ensemble/CombatEnd3/ensemble1_tambo_combat_End3_90_7-8.ogg",
	"sound/ensemble/CombatEnd3/ensemble1_tambo_combat_End3_90_7-8.ogg",
	"sound/ensemble/CombatEnd3/ensemble1_tambo_combat_End3_90_7-8.ogg",
	"sound/ensemble/CombatEnd4/ensemble1_drum_combat_End4_90_7-8.ogg",
	"sound/ensemble/CombatEnd4/ensemble1_flute_combat_End4_90_7-8.ogg",
	"sound/ensemble/CombatEnd4/ensemble1_horn_combat_End4_90_7-8.ogg",
	"sound/ensemble/CombatEnd4/ensemble1_lute_combat_End4_90_7-8.ogg",
	"sound/ensemble/CombatEnd4/ensemble1_lyre_combat_End4_90_7-8.ogg",
	"sound/ensemble/CombatEnd4/ensemble1_tambo_combat_End4_90_7-8.ogg",
	"sound/ensemble/CombatEnd4/ensemble1_tambo_combat_End4_90_7-8.ogg",
	"sound/ensemble/CombatEnd4/ensemble1_tambo_combat_End4_90_7-8.ogg",*/
	"sound/ensemble/Trans4/ensemble1_drum_Trans_120_4-4.ogg",
	"sound/ensemble/Trans4/ensemble1_flute_Trans_120_4-4.ogg",
	"sound/ensemble/Trans4/ensemble1_horn_Trans_120_4-4.ogg",
	"sound/ensemble/Trans4/ensemble1_lute_Trans_120_4-4.ogg",
	"sound/ensemble/Trans4/ensemble1_lyre_Trans_120_4-4.ogg",
	"sound/ensemble/Trans4/ensemble1_tambo_Trans_120_4-4.ogg",
	"sound/ensemble/Trans4/ensemble1_BEB_tier1_Trans_120_4-4.ogg",
	"sound/ensemble/Trans4/ensemble1_BEB_tier2_Trans_120_4-4.ogg"
};

bool physfsSearchMusicToUpdate()
{
	if ( no_sound )
	{
		return false;
	}
#ifdef SOUND

	for ( auto it = themeMusic.begin(); it != themeMusic.end(); ++it )
	{
		std::string filename = *it;
		if ( PHYSFS_getRealDir(filename.c_str()) != nullptr )
		{
			std::string musicDir = PHYSFS_getRealDir(filename.c_str());
			if ( musicDir.compare("./") != 0 )
			{
				printlog("[PhysFS]: Found modified music in music/ directory, reloading music files...");
				return true;
			}
		}
	}

	int c;

	if ( physfsSearchMusicToUpdate_helper_findModifiedMusic(NUMMINESMUSIC, "music/mines%02d.ogg")
		|| physfsSearchMusicToUpdate_helper_findModifiedMusic(NUMSWAMPMUSIC, "music/swamp%02d.ogg")
		|| physfsSearchMusicToUpdate_helper_findModifiedMusic(NUMLABYRINTHMUSIC, "music/labyrinth%02d.ogg")
		|| physfsSearchMusicToUpdate_helper_findModifiedMusic(NUMRUINSMUSIC, "music/ruins%02d.ogg")
		|| physfsSearchMusicToUpdate_helper_findModifiedMusic(NUMUNDERWORLDMUSIC, "music/underworld%02d.ogg")
		|| physfsSearchMusicToUpdate_helper_findModifiedMusic(NUMHELLMUSIC, "music/hell%02d.ogg")
		|| physfsSearchMusicToUpdate_helper_findModifiedMusic(NUMMINOTAURMUSIC, "music/minotaur%02d.ogg")
		|| physfsSearchMusicToUpdate_helper_findModifiedMusic(NUMCAVESMUSIC, "music/caves%02d.ogg")
		|| physfsSearchMusicToUpdate_helper_findModifiedMusic(NUMCITADELMUSIC, "music/citadel%02d.ogg")
#ifdef USE_FMOD
        || physfsSearchMusicToUpdate_helper_findModifiedMusic(NUMFORTRESSMUSIC, "music/fortress%02d.ogg")
#endif
        )
	{
		return true;
	}

	for ( c = 0; c < NUMINTROMUSIC; c++ )
	{
		if ( c == 0 )
		{
			strcpy(tempstr, "music/intro.ogg");
		}
		else
		{
			snprintf(tempstr, 1000, "music/intro%02d.ogg", c);
		}
		if ( PHYSFS_getRealDir(tempstr) != nullptr )
		{
			std::string musicDir = PHYSFS_getRealDir(tempstr);
			if ( musicDir.compare("./") != 0 )
			{
				printlog("[PhysFS]: Found modified music in music/ directory, reloading music files...");
				return true;
			}
		}
	}
#endif // SOUND
	return false;
}

#ifdef USE_FMOD
FMOD_RESULT physfsReloadMusic_helper_reloadMusicArray(uint32_t numMusic, const char* filenameTemplate, FMOD::Sound** musicArray, bool reloadAll)
{
	for ( int c = 0; c < numMusic; c++ )
	{
		snprintf(tempstr, 1000, filenameTemplate, c);
		if ( PHYSFS_getRealDir(tempstr) != nullptr )
		{
			std::string musicDir = PHYSFS_getRealDir(tempstr);
			if ( musicDir.compare("./") != 0 || reloadAll )
			{
				musicDir.append(PHYSFS_getDirSeparator()).append(tempstr);
				printlog("[PhysFS]: Loading music file %s...", tempstr);
				if ( musicArray )
				{
					musicArray[c]->release();
				}
                if ( musicPreload )
                {
                    fmod_result = fmod_system->createSound(musicDir.c_str(), FMOD_2D, nullptr, &musicArray[c]); //TODO: Any other FMOD_MODEs should be used here? FMOD_SOFTWARE -> what now? FMOD_2D? LOOP?
                }
                else
                {
                    fmod_result = fmod_system->createStream(musicDir.c_str(), FMOD_2D, nullptr, &musicArray[c]); //TODO: Any other FMOD_MODEs should be used here? FMOD_SOFTWARE -> what now? FMOD_2D? LOOP?
                }
                if (fmod_result != FMOD_OK)
                {
                    printlog("[PhysFS]: ERROR: Failed reloading music file \"%s\".");
                    return fmod_result;
                }
			}
		}
	}

	return FMOD_OK;
}
#endif

void physfsReloadMusic(bool &introMusicChanged, bool reloadAll) //TODO: This should probably return an error.
{
	if ( no_sound )
	{
		return;
	}
#ifdef SOUND
#ifdef USE_OPENAL
    physfsReloadMusicOpenAL(introMusicChanged, reloadAll);
#else
    int index = 0;
	bool ensembleNeedsUpdate = false;
	for ( auto it = themeMusic.begin(); it != themeMusic.end(); ++it )
	{
		std::string filename = *it;
		if ( PHYSFS_getRealDir(filename.c_str()) != nullptr )
		{
			std::string musicDir = PHYSFS_getRealDir(filename.c_str());
			if ( musicDir.compare("./") != 0 || reloadAll )
			{
				musicDir += PHYSFS_getDirSeparator() + filename;
				printlog("[PhysFS]: Loading music file %s...", filename.c_str());
				switch ( index )
				{
					case 0:
						if ( introductionmusic )
						{
							introductionmusic->release();
						}
                        if ( musicPreload )
                        {
                            fmod_result = fmod_system->createSound(musicDir.c_str(), FMOD_2D, nullptr, &introductionmusic); //TODO: FMOD_SOFTWARE -> what now? FMOD_2D? FMOD_LOOP_NORMAL? More things? Something else?
                        }
                        else
                        {
                            fmod_result = fmod_system->createStream(musicDir.c_str(), FMOD_2D, nullptr, &introductionmusic); //TODO: FMOD_SOFTWARE -> what now? FMOD_2D? FMOD_LOOP_NORMAL? More things? Something else?
                        }
						break;
					case 1:
						if ( intermissionmusic )
						{
							intermissionmusic->release();
						}
                        if ( musicPreload )
                        {
                            fmod_result = fmod_system->createSound(musicDir.c_str(), FMOD_2D, nullptr, &intermissionmusic);
                        }
                        else
                        {
                            fmod_result = fmod_system->createStream(musicDir.c_str(), FMOD_2D, nullptr, &intermissionmusic);
                        }
						break;
					case 2:
						if ( minetownmusic )
						{
							minetownmusic->release();
						}
                        if ( musicPreload )
                        {
                            fmod_result = fmod_system->createSound(musicDir.c_str(), FMOD_2D, nullptr, &minetownmusic);
                        }
                        else
                        {
                            fmod_result = fmod_system->createStream(musicDir.c_str(), FMOD_2D, nullptr, &minetownmusic);
                        }
						break;
					case 3:
						if ( splashmusic )
						{
							splashmusic->release();
						}
                        if ( musicPreload )
                        {
                            fmod_result = fmod_system->createSound(musicDir.c_str(), FMOD_2D, nullptr, &splashmusic);
                        }
                        else
                        {
                            fmod_result = fmod_system->createStream(musicDir.c_str(), FMOD_2D, nullptr, &splashmusic);
                        }
						break;
					case 4:
						if ( librarymusic )
						{
							librarymusic->release();
						}
                        if ( musicPreload )
                        {
                            fmod_result = fmod_system->createSound(musicDir.c_str(), FMOD_2D, nullptr, &librarymusic);
                        }
                        else
                        {
                            fmod_result = fmod_system->createStream(musicDir.c_str(), FMOD_2D, nullptr, &librarymusic);
                        }
						break;
					case 5:
						if ( shopmusic )
						{
							shopmusic->release();
						}
                        if ( musicPreload )
                        {
                            fmod_result = fmod_system->createSound(musicDir.c_str(), FMOD_2D, nullptr, &shopmusic);
                        }
                        else
                        {
                            fmod_result = fmod_system->createStream(musicDir.c_str(), FMOD_2D, nullptr, &shopmusic);
                        }
						break;
					case 6:
						if ( herxmusic )
						{
							herxmusic->release();
						}
                        if ( musicPreload )
                        {
                            fmod_result = fmod_system->createSound(musicDir.c_str(), FMOD_2D, nullptr, &herxmusic);
                        }
                        else
                        {
                            fmod_result = fmod_system->createStream(musicDir.c_str(), FMOD_2D, nullptr, &herxmusic);
                        }
						break;
					case 7:
						if ( templemusic )
						{
							templemusic->release();
						}
                        if ( musicPreload )
                        {
                            fmod_result = fmod_system->createSound(musicDir.c_str(), FMOD_2D, nullptr, &templemusic);
                        }
                        else
                        {
                            fmod_result = fmod_system->createStream(musicDir.c_str(), FMOD_2D, nullptr, &templemusic);
                        }
						break;
					case 8:
						if ( endgamemusic )
						{
							endgamemusic->release();
						}
                        if ( musicPreload )
                        {
                            fmod_result = fmod_system->createSound(musicDir.c_str(), FMOD_2D, nullptr, &endgamemusic);
                        }
                        else
                        {
                            fmod_result = fmod_system->createStream(musicDir.c_str(), FMOD_2D, nullptr, &endgamemusic);
                        }
						break;
					case 9:
						if ( escapemusic )
						{
							escapemusic->release();
						}
                        if ( musicPreload )
                        {
                            fmod_result = fmod_system->createSound(musicDir.c_str(), FMOD_2D, nullptr, &escapemusic);
                        }
                        else
                        {
                            fmod_result = fmod_system->createStream(musicDir.c_str(), FMOD_2D, nullptr, &escapemusic);
                        }
						break;
					case 10:
						if ( devilmusic )
						{
							devilmusic->release();
						}
                        if ( musicPreload )
                        {
                            fmod_result = fmod_system->createSound(musicDir.c_str(), FMOD_2D, nullptr, &devilmusic);
                        }
                        else
                        {
                            fmod_result = fmod_system->createStream(musicDir.c_str(), FMOD_2D, nullptr, &devilmusic);
                        }
						break;
					case 11:
						if ( sanctummusic )
						{
							sanctummusic->release();
                        }
                        if ( musicPreload )
                        {
                            fmod_result = fmod_system->createSound(musicDir.c_str(), FMOD_2D, nullptr, &sanctummusic);
                        }
                        else
                        {
                            fmod_result = fmod_system->createStream(musicDir.c_str(), FMOD_2D, nullptr, &sanctummusic);
                        }
						break;
					case 12:
						if ( gnomishminesmusic )
						{
							gnomishminesmusic->release();
						}
						if ( musicPreload )
						{
							fmod_result = fmod_system->createSound(musicDir.c_str(), FMOD_2D, nullptr, &gnomishminesmusic);
						}
						else
						{
							fmod_result = fmod_system->createStream(musicDir.c_str(), FMOD_2D, nullptr, &gnomishminesmusic);
						}
						break;
					case 13:
						if ( greatcastlemusic )
						{
							greatcastlemusic->release();
						}
						if ( musicPreload )
						{
							fmod_result = fmod_system->createSound(musicDir.c_str(), FMOD_2D, nullptr, &greatcastlemusic);
						}
						else
						{
							fmod_result = fmod_system->createStream(musicDir.c_str(), FMOD_2D, nullptr, &greatcastlemusic);
						}
						break;
					case 14:
						if ( sokobanmusic )
						{
							sokobanmusic->release();
						}
						if ( musicPreload )
						{
							fmod_result = fmod_system->createSound(musicDir.c_str(), FMOD_2D, nullptr, &sokobanmusic);
						}
						else
						{
							fmod_result = fmod_system->createStream(musicDir.c_str(), FMOD_2D, nullptr, &sokobanmusic);
						}
						break;
					case 15:
						if ( caveslairmusic )
						{
							caveslairmusic->release();
						}
						if ( musicPreload )
						{
							fmod_result = fmod_system->createSound(musicDir.c_str(), FMOD_2D, nullptr, &caveslairmusic);
						}
						else
						{
							fmod_result = fmod_system->createStream(musicDir.c_str(), FMOD_2D, nullptr, &caveslairmusic);
						}
						break;
					case 16:
						if ( bramscastlemusic )
						{
							bramscastlemusic->release();
						}
						if ( musicPreload )
						{
							fmod_result = fmod_system->createSound(musicDir.c_str(), FMOD_2D, nullptr, &bramscastlemusic);
						}
						else
						{
							fmod_result = fmod_system->createStream(musicDir.c_str(), FMOD_2D, nullptr, &bramscastlemusic);
						}
						break;
					case 17:
						if ( hamletmusic )
						{
							hamletmusic->release();
						}
						if ( musicPreload )
						{
							fmod_result = fmod_system->createSound(musicDir.c_str(), FMOD_2D, nullptr, &hamletmusic);
						}
						else
						{
							fmod_result = fmod_system->createStream(musicDir.c_str(), FMOD_2D, nullptr, &hamletmusic);
						}
						break;
					case 18:
						if ( tutorialmusic )
						{
							tutorialmusic->release();
						}
						if ( musicPreload )
						{
							fmod_result = fmod_system->createSound(musicDir.c_str(), FMOD_2D, nullptr, &tutorialmusic);
						}
						else
						{
							fmod_result = fmod_system->createStream(musicDir.c_str(), FMOD_2D, nullptr, &tutorialmusic);
						}
						break;
					case 19:
						if ( gameovermusic )
						{
							gameovermusic->release();
						}
						if ( musicPreload )
						{
							fmod_result = fmod_system->createSound(musicDir.c_str(), FMOD_DEFAULT, nullptr, &gameovermusic);
						}
						else
						{
							fmod_result = fmod_system->createStream(musicDir.c_str(), FMOD_DEFAULT, nullptr, &gameovermusic);
						}
						break;
					case 20:
						if ( introstorymusic )
						{
							introstorymusic->release();
						}
						if ( musicPreload )
						{
							fmod_result = fmod_system->createSound(musicDir.c_str(), FMOD_DEFAULT, nullptr, &introstorymusic);
						}
						else
						{
							fmod_result = fmod_system->createStream(musicDir.c_str(), FMOD_DEFAULT, nullptr, &introstorymusic);
						}
						break;
					default:
#ifdef USE_FMOD
#ifndef EDITOR
						if ( index >= 21 && index < 21 + NUMENSEMBLEMUSIC * 5 )
						{
#ifdef NINTENDO
							if ( !ensembleSounds.firstTimeSetup )
							{
								continue;
							}
#endif

							ensembleNeedsUpdate = true;
							int c = (index - 21) % NUMENSEMBLEMUSIC;
							FMOD_MODE flags = FMOD_3D | FMOD_LOOP_NORMAL | FMOD_NONBLOCKING;
#ifdef NINTENDO
							flags |= FMOD_NONBLOCKING;
#endif
							if ( index >= 21 + NUMENSEMBLEMUSIC * 0 && index < 21 + NUMENSEMBLEMUSIC * 1 )
							{
								fmod_result = ensembleSounds.exploreChannel[c] ? ensembleSounds.exploreChannel[c]->stop() : FMOD_OK;
								fmod_result = ensembleSounds.exploreSound[c] ? ensembleSounds.exploreSound[c]->release() : FMOD_OK;
								fmod_result = fmod_system->createSound(musicDir.c_str(), flags, nullptr, &ensembleSounds.exploreSound[c]);
							}
							else if ( index >= 21 + NUMENSEMBLEMUSIC * 1 && index < 21 + NUMENSEMBLEMUSIC * 2 )
							{
								fmod_result = ensembleSounds.combatChannel[c] ? ensembleSounds.combatChannel[c]->stop() : FMOD_OK;
								fmod_result = ensembleSounds.combatSound[c] ? ensembleSounds.combatSound[c]->release() : FMOD_OK;
								fmod_result = fmod_system->createSound(musicDir.c_str(), flags, nullptr, &ensembleSounds.combatSound[c]);
							}
							else if ( index >= 21 + NUMENSEMBLEMUSIC * 2 && index < 21 + NUMENSEMBLEMUSIC * 3 )
							{
								fmod_result = ensembleSounds.combatTransChannel[0][c] ? ensembleSounds.combatTransChannel[0][c]->stop() : FMOD_OK;
								fmod_result = ensembleSounds.combatTransSound[0][c] ? ensembleSounds.combatTransSound[0][c]->release() : FMOD_OK;
								fmod_result = fmod_system->createSound(musicDir.c_str(), flags, nullptr, &ensembleSounds.combatTransSound[0][c]);
							}
							else if ( index >= 21 + NUMENSEMBLEMUSIC * 3 && index < 21 + NUMENSEMBLEMUSIC * 4 )
							{
								fmod_result = ensembleSounds.combatTransChannel[1][c] ? ensembleSounds.combatTransChannel[1][c]->stop() : FMOD_OK;
								fmod_result = ensembleSounds.combatTransSound[1][c] ? ensembleSounds.combatTransSound[1][c]->release() : FMOD_OK;
								fmod_result = fmod_system->createSound(musicDir.c_str(), flags, nullptr, &ensembleSounds.combatTransSound[1][c]);
							}
							else if ( index >= 21 + NUMENSEMBLEMUSIC * 4 && index < 21 + NUMENSEMBLEMUSIC * 5 )
							{
								fmod_result = ensembleSounds.exploreTransChannel[3][c] ? ensembleSounds.exploreTransChannel[3][c]->stop() : FMOD_OK;
								fmod_result = ensembleSounds.exploreTransSound[3][c] ? ensembleSounds.exploreTransSound[3][c]->release() : FMOD_OK;
								fmod_result = fmod_system->createSound(musicDir.c_str(), flags, nullptr, &ensembleSounds.exploreTransSound[3][c]);
							}
							/*else if ( index >= 21 + NUMENSEMBLEMUSIC * 2 && index < 21 + NUMENSEMBLEMUSIC * 3 )
							{
								fmod_result = ensembleSounds.exploreTransChannel[0][c] ? ensembleSounds.exploreTransChannel[0][c]->stop() : FMOD_OK;
								fmod_result = ensembleSounds.exploreTransSound[0][c] ? ensembleSounds.exploreTransSound[0][c]->release() : FMOD_OK;
								fmod_result = fmod_system->createSound(musicDir.c_str(), FMOD_3D | FMOD_LOOP_NORMAL, nullptr, &ensembleSounds.exploreTransSound[0][c]);
							}
							else if ( index >= 21 + NUMENSEMBLEMUSIC * 3 && index < 21 + NUMENSEMBLEMUSIC * 4 )
							{
								fmod_result = ensembleSounds.exploreTransChannel[1][c] ? ensembleSounds.exploreTransChannel[1][c]->stop() : FMOD_OK;
								fmod_result = ensembleSounds.exploreTransSound[1][c] ? ensembleSounds.exploreTransSound[1][c]->release() : FMOD_OK;
								fmod_result = fmod_system->createSound(musicDir.c_str(), FMOD_3D | FMOD_LOOP_NORMAL, nullptr, &ensembleSounds.exploreTransSound[1][c]);
							}
							else if ( index >= 21 + NUMENSEMBLEMUSIC * 4 && index < 21 + NUMENSEMBLEMUSIC * 5 )
							{
								fmod_result = ensembleSounds.exploreTransChannel[2][c] ? ensembleSounds.exploreTransChannel[2][c]->stop() : FMOD_OK;
								fmod_result = ensembleSounds.exploreTransSound[2][c] ? ensembleSounds.exploreTransSound[2][c]->release() : FMOD_OK;
								fmod_result = fmod_system->createSound(musicDir.c_str(), FMOD_3D | FMOD_LOOP_NORMAL, nullptr, &ensembleSounds.exploreTransSound[2][c]);
							}
							else if ( index >= 21 + NUMENSEMBLEMUSIC * 5 && index < 21 + NUMENSEMBLEMUSIC * 6 )
							{
								fmod_result = ensembleSounds.combatTransChannel[0][c] ? ensembleSounds.combatTransChannel[0][c]->stop() : FMOD_OK;
								fmod_result = ensembleSounds.combatTransSound[0][c] ? ensembleSounds.combatTransSound[0][c]->release() : FMOD_OK;
								fmod_result = fmod_system->createSound(musicDir.c_str(), FMOD_3D | FMOD_LOOP_NORMAL, nullptr, &ensembleSounds.combatTransSound[0][c]);
							}
							else if ( index >= 21 + NUMENSEMBLEMUSIC * 6 && index < 21 + NUMENSEMBLEMUSIC * 7 )
							{
								fmod_result = ensembleSounds.combatTransChannel[1][c] ? ensembleSounds.combatTransChannel[1][c]->stop() : FMOD_OK;
								fmod_result = ensembleSounds.combatTransSound[1][c] ? ensembleSounds.combatTransSound[1][c]->release() : FMOD_OK;
								fmod_result = fmod_system->createSound(musicDir.c_str(), FMOD_3D | FMOD_LOOP_NORMAL, nullptr, &ensembleSounds.combatTransSound[1][c]);
							}
							else if ( index >= 21 + NUMENSEMBLEMUSIC * 7 && index < 21 + NUMENSEMBLEMUSIC * 8 )
							{
								fmod_result = ensembleSounds.combatTransChannel[2][c] ? ensembleSounds.combatTransChannel[2][c]->stop() : FMOD_OK;
								fmod_result = ensembleSounds.combatTransSound[2][c] ? ensembleSounds.combatTransSound[2][c]->release() : FMOD_OK;
								fmod_result = fmod_system->createSound(musicDir.c_str(), FMOD_3D | FMOD_LOOP_NORMAL, nullptr, &ensembleSounds.combatTransSound[2][c]);
							}
							else if ( index >= 21 + NUMENSEMBLEMUSIC * 8 && index < 21 + NUMENSEMBLEMUSIC * 9 )
							{
								fmod_result = ensembleSounds.combatTransChannel[3][c] ? ensembleSounds.combatTransChannel[3][c]->stop() : FMOD_OK;
								fmod_result = ensembleSounds.combatTransSound[3][c] ? ensembleSounds.combatTransSound[3][c]->release() : FMOD_OK;
								fmod_result = fmod_system->createSound(musicDir.c_str(), FMOD_3D | FMOD_LOOP_NORMAL, nullptr, &ensembleSounds.combatTransSound[3][c]);
							}
							else if ( index >= 21 + NUMENSEMBLEMUSIC * 9 && index < 21 + NUMENSEMBLEMUSIC * 10 )
							{
								fmod_result = ensembleSounds.exploreTransChannel[3][c] ? ensembleSounds.exploreTransChannel[3][c]->stop() : FMOD_OK;
								fmod_result = ensembleSounds.exploreTransSound[3][c] ? ensembleSounds.exploreTransSound[3][c]->release() : FMOD_OK;
								fmod_result = fmod_system->createSound(musicDir.c_str(), FMOD_3D | FMOD_LOOP_NORMAL, nullptr, &ensembleSounds.exploreTransSound[3][c]);
							}*/
						}
#endif
#endif
						break;
				}
				if ( FMODErrorCheck() )
				{
					printlog("[PhysFS]: ERROR: Failed reloading music file \"%s\".", filename.c_str());
					//TODO: Handle error? Abort? Fling pies at people?
				}
			}
		}
		++index;
	}

	int c;
	FMOD::Sound** music = nullptr;

	if (FMOD_OK != (fmod_result = physfsReloadMusic_helper_reloadMusicArray(NUMMINESMUSIC, "music/mines%02d.ogg", minesmusic, reloadAll)) )
	{
		printlog("[PhysFS]: Failed to reload mines music array.");
		//TODO: Handle error? Abort? Fling pies at people?
	}
	if (FMOD_OK != (fmod_result = physfsReloadMusic_helper_reloadMusicArray(NUMSWAMPMUSIC, "music/swamp%02d.ogg", swampmusic, reloadAll)) )
	{
		printlog("[PhysFS]: Failed to reload swamp music array.");
	}
	if (FMOD_OK != (fmod_result = physfsReloadMusic_helper_reloadMusicArray(NUMLABYRINTHMUSIC, "music/labyrinth%02d.ogg", labyrinthmusic, reloadAll)) )
	{
		printlog("[PhysFS]: Failed to reload labyrinth music array.");
	}
	if (FMOD_OK != (fmod_result = physfsReloadMusic_helper_reloadMusicArray(NUMRUINSMUSIC, "music/ruins%02d.ogg", ruinsmusic, reloadAll)) )
	{
		printlog("[PhysFS]: Failed to reload ruins music array.");
	}
	if (FMOD_OK != (fmod_result = physfsReloadMusic_helper_reloadMusicArray(NUMUNDERWORLDMUSIC, "music/underworld%02d.ogg", underworldmusic, reloadAll)) )
	{
		printlog("[PhysFS]: Failed to reload underworld music array.");
	}
	if (FMOD_OK != (fmod_result = physfsReloadMusic_helper_reloadMusicArray(NUMHELLMUSIC, "music/hell%02d.ogg", hellmusic, reloadAll)) )
	{
		printlog("[PhysFS]: Failed to reload hell music array.");
	}
	if (FMOD_OK != (fmod_result = physfsReloadMusic_helper_reloadMusicArray(NUMMINOTAURMUSIC, "music/minotaur%02d.ogg", minotaurmusic, reloadAll)) )
	{
		printlog("[PhysFS]: Failed to reload minotaur music array.");
	}
	if (FMOD_OK != (fmod_result = physfsReloadMusic_helper_reloadMusicArray(NUMCAVESMUSIC, "music/caves%02d.ogg", cavesmusic, reloadAll)) )
	{
		printlog("[PhysFS]: Failed to reload caves music array.");
	}
	if (FMOD_OK != (fmod_result = physfsReloadMusic_helper_reloadMusicArray(NUMCITADELMUSIC, "music/citadel%02d.ogg", citadelmusic, reloadAll)) )
	{
		printlog("[PhysFS]: Failed to reload citadel music array.");
	}
	if ( FMOD_OK != (fmod_result = physfsReloadMusic_helper_reloadMusicArray(NUMFORTRESSMUSIC, "music/fortress%02d.ogg", fortressmusic, reloadAll)) )
	{
		printlog("[PhysFS]: Failed to reload fortress music array.");
	}

	bool introChanged = false;

	for ( c = 0; c < NUMINTROMUSIC; c++ )
	{
		if ( c == 0 )
		{
			strcpy(tempstr, "music/intro.ogg");
		}
		else
		{
			snprintf(tempstr, 1000, "music/intro%02d.ogg", c);
		}
		if ( PHYSFS_getRealDir(tempstr) != nullptr )
		{
			std::string musicDir = PHYSFS_getRealDir(tempstr);
			if ( musicDir.compare("./") != 0 || reloadAll )
			{
				musicDir.append(PHYSFS_getDirSeparator()).append(tempstr);
				printlog("[PhysFS]: Loading music file %s...", tempstr);
				music = intromusic;
				if ( music )
				{
					music[c]->release();
				}
                if ( musicPreload )
                {
                    fmod_result = fmod_system->createSound(musicDir.c_str(), FMOD_2D, nullptr, &music[c]);
                }
                else
                {
                    fmod_result = fmod_system->createStream(musicDir.c_str(), FMOD_2D, nullptr, &music[c]);
                }
                introChanged = true;
                if (fmod_result != FMOD_OK)
                {
                    printlog("[PhysFS]: ERROR: Failed reloading music file \"%s\".");
                    break; //TODO: Handle the error?
                }
			}
		}
	}

#ifdef USE_FMOD
#ifndef EDITOR
	if ( ensembleNeedsUpdate && !ensembleSounds.firstTimeSetup ) // only setup here on modded reloads
	{
		ensembleSounds.setup();
	}
#endif
#endif

	introMusicChanged = introChanged; // use this variable outside of this function to start playing a new fresh list of tracks in the main menu.
#endif // USE_OPENAL
#endif // SOUND
}

void gamemodsUnloadCustomThemeMusic()
{
#ifdef SOUND
	// free custom music slots, not used by official music assets.
	if ( gnomishminesmusic )
	{
#ifdef USE_FMOD
		gnomishminesmusic->release();
#else
        OPENAL_Sound_Release(gnomishminesmusic);
#endif
		gnomishminesmusic = nullptr;
	}
	if ( greatcastlemusic )
	{
#ifdef USE_FMOD
		greatcastlemusic->release();
#else
        OPENAL_Sound_Release(greatcastlemusic);
#endif
		greatcastlemusic = nullptr;
	}
	if ( sokobanmusic )
	{
#ifdef USE_FMOD
		sokobanmusic->release();
#else
        OPENAL_Sound_Release(sokobanmusic);
#endif
		sokobanmusic = nullptr;
	}
	if ( caveslairmusic )
	{
#ifdef USE_FMOD
		caveslairmusic->release();
#else
        OPENAL_Sound_Release(caveslairmusic);
#endif
		caveslairmusic = nullptr;
	}
	if ( bramscastlemusic )
	{
#ifdef USE_FMOD
		bramscastlemusic->release();
#else
        OPENAL_Sound_Release(bramscastlemusic);
#endif
		bramscastlemusic = nullptr;
	}
	if ( hamletmusic )
	{
#ifdef USE_FMOD
		hamletmusic->release();
#else
        OPENAL_Sound_Release(hamletmusic);
#endif
		hamletmusic = nullptr;
	}
#endif // !SOUND
}
