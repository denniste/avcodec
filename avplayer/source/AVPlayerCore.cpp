#include "AVPlayerCore.h"
#include "sys/system.h"
#include <stdio.h>
#include <stdlib.h>
#include <memory.h>
#include <assert.h>
#include <errno.h>
#include "ctypedef.h"
#include "avlog.h"

inline int v_min(int x, int y)
{
	return x < y ? x : y;
}

#define TIMEOUT_DEFAULT 20
#define AUDIO_SAMPLE_THRESHOLD 100 // ms

AVPlayerCore::AVPlayerCore(const avplayer_notify_t* notify, void* param)
	: m_notify(*notify), m_notify_param(param)
	, m_status(avplayer_status_close)
	, m_buffering(true)
{
	memset(&m_video, 0, sizeof(m_video));
	memset(&m_audio, 0, sizeof(m_audio));
	memset(&m_vclock, 0, sizeof(m_vclock));
	memset(&m_aclock, 0, sizeof(m_aclock));
	memset(&m_system, 0, sizeof(m_system));

	m_running = true;
	thread_create(&m_thread, OnThread, this);
}

AVPlayerCore::~AVPlayerCore()
{
	m_running = false;
	m_event.Signal(); // notify to exit
	thread_destroy(m_thread);

	// clear audio/video frames
	if (m_video.frame) m_notify.on_video(m_notify_param, m_video.frame, 1);
	if (m_audio.frame) m_notify.on_audio(m_notify_param, m_audio.frame, 1);
	while(m_videoQ.Read(m_video)) m_notify.on_video(m_notify_param, m_video.frame, 1);
	while(m_audioQ.Read(m_audio)) m_notify.on_audio(m_notify_param, m_audio.frame, 1);
}

void AVPlayerCore::Play()
{
	memset(&m_vclock, 0, sizeof(m_vclock));
	memset(&m_aclock, 0, sizeof(m_aclock));
	m_status = avplayer_status_play;
	m_event.Signal();
}

void AVPlayerCore::Pause()
{
	m_status = avplayer_status_pause;
	m_event.Signal();
}

void AVPlayerCore::Stop()
{
	m_status = avplayer_status_stop;
	m_event.Signal();
}

void AVPlayerCore::Input(const void* pcm, uint64_t pts, uint64_t duration, int serial)
{
	AVFrame audio;
	audio.pts = pts;
	audio.frame = pcm;
	audio.serial = serial;
	audio.duration = duration;
	m_audioQ.Write(audio);
}

void AVPlayerCore::Input(const void* yuv, uint64_t pts, int serial)
{
	AVFrame video;
	video.pts = pts;
	video.frame = yuv;
	video.serial = serial;
	video.duration = 0;
	m_videoQ.Write(video);
}

int AVPlayerCore::OnThread(void* param)
{
	AVPlayerCore* self = (AVPlayerCore*)param;
	return self->OnThread();
}

int AVPlayerCore::OnThread()
{
	int timeout = 100;
	while (m_running)
	{
		if (WAIT_TIMEOUT == m_event.TimeWait(timeout))
		{
			if (avplayer_status_play == m_status)
			{
				timeout = OnPlay(system_clock());
			}
		}
	}
	return 0;
}

int AVPlayerCore::OnPlay(uint64_t clock)
{
	if (m_buffering)
	{
		if (m_videoQ.Size() < 3 && m_audioQ.GetDuration() < 100)
			return TIMEOUT_DEFAULT;
	}

	if (NULL == m_video.frame) m_videoQ.Read(m_video);
	if (NULL == m_audio.frame) m_audioQ.Read(m_audio);
	if (NULL == m_video.frame && NULL == m_audio.frame)
	{
		m_buffering = true;
		m_notify.on_buffering(m_notify_param, 1);
		return TIMEOUT_DEFAULT;
	}

	if (m_buffering)
	{
		m_buffering = false;
		m_notify.on_buffering(m_notify_param, 0);
	}

	int timeout = TIMEOUT_DEFAULT;
	if (NULL != m_audio.frame)
		timeout = v_min(OnAudio(clock), timeout);
	if(NULL != m_video.frame)
		timeout = v_min(OnVideo(clock), timeout);
	return timeout;
}

int AVPlayerCore::OnVideo(uint64_t clock)
{
	assert(m_video.frame);
	if (m_video.serial != m_videoQ.GetSerial())
	{
		m_notify.on_video(m_notify_param, m_video.frame, 1);
		m_video.frame = NULL;
		return 0; // next frame
	}

	uint64_t diff = m_video.pts - m_vclock.pts;
	if (diff > 20 * 1000) diff = 40;

	if (clock < m_vclock.frame_time + diff)
	{
		return (int)(m_vclock.frame_time + diff - clock); // remain time
	}

	m_vclock.frame_time += diff;
	if (clock - m_vclock.frame_time > 100)
	{
		avlog("video clock reset: v-clock: %" PRIu64 " -> %" PRIu64 "\n", m_vclock.frame_time, clock);
		m_vclock.frame_time = clock;
	}

	m_vclock.clock = clock;
	m_vclock.pts = m_video.pts;
	const void* frame = m_video.frame;
	m_video.frame = NULL;

	// draw frame
	m_notify.on_video(m_notify_param, frame, 0);

	avlog("Video: v-pts: %" PRIu64 ", v-clock: %" PRIu64 ", v-diff: %" PRId64 "\n", m_vclock.pts, m_vclock.clock, m_vclock.clock-m_vclock.frame_time);
	return 0; // draw next frame
}

int AVPlayerCore::OnAudio(uint64_t clock)
{
	assert(m_audio.frame);
	if (m_audio.serial != m_audioQ.GetSerial())
	{
		m_notify.on_audio(m_notify_param, m_audio.frame, 1);
		m_audio.frame = NULL;
		return 0; // next frame
	}

	// remain audio sample duration(predict)
	int64_t duration = (int64_t)(m_aclock.frame_time - (clock - m_aclock.clock));
	if (duration > AUDIO_SAMPLE_THRESHOLD)
	{
		return AUDIO_SAMPLE_THRESHOLD/2;
	}

	m_aclock.clock = clock;
	m_aclock.pts = m_aclock.duration > 0 ? m_aclock.pts + m_aclock.duration : m_audio.pts;
	m_aclock.duration = m_audio.duration;
	const void* pcm = m_audio.frame;
	m_audio.frame = NULL;
	
	// play audio(write to audio buffer)
	m_aclock.frame_time = m_notify.on_audio(m_notify_param, pcm, 0);
	AVSync(clock); // audio sync video
	return 0; // next frame
}

int AVPlayerCore::AVSync(uint64_t clock)
{
	// current audio playing pts
	if (m_audio.pts + m_audio.duration < m_aclock.frame_time)
	{
		avlog("AVSync: audio pts: %" PRIu64 ", duration: %" PRIu64 ", frame_time: %" PRIu64 "\n", m_audio.pts, m_audio.duration, m_aclock.frame_time);
		//assert(0);
		return 0;
	}

	m_system.pts = m_audio.pts + m_audio.duration - m_aclock.frame_time;
	m_system.clock = clock;

	if (m_system.clock - m_vclock.frame_time > 100)
	{
		avlog("AVSync: v-pts: %" PRIu64 " -> %" PRIu64 ", v-clock: %" PRIu64 " -> %" PRIu64 "\n", m_vclock.pts, m_system.pts, m_vclock.frame_time, m_system.clock);
		m_vclock.pts = m_system.pts;
		m_vclock.frame_time = m_system.clock;
	}

	return 0;
}
