/*
 *  Copyright (C) 2005-2020 Team Kodi
 *  https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSE.md for more information.
 */

#include "FFmpegCatchupStream.h"

#include "threads/SingleLock.h"
#include "../utils/Log.h"

using namespace ffmpegdirect::utils;

#ifdef TARGET_POSIX
#include "platform/posix/XTimeUtils.h"
#endif

#ifndef __STDC_CONSTANT_MACROS
#define __STDC_CONSTANT_MACROS
#endif
#ifndef __STDC_LIMIT_MACROS
#define __STDC_LIMIT_MACROS
#endif
#ifdef TARGET_POSIX
#include <stdint.h>
#endif

extern "C" {
#include <libavutil/dict.h>
#include <libavutil/opt.h>
}

#include <regex>

//#include "platform/posix/XTimeUtils.h"

#include <p8-platform/util/StringUtils.h>

/***********************************************************
* InputSteam Client AddOn specific public library functions
***********************************************************/

FFmpegCatchupStream::FFmpegCatchupStream(IManageDemuxPacket* demuxPacketManager,
                                         const HttpProxy& httpProxy,
                                         std::string& defaultUrl,
                                         bool playbackAsLive,
                                         time_t programmeStartTime,
                                         time_t programmeEndTime,
                                         std::string& catchupUrlFormatString,
                                         std::string& catchupUrlNearLiveFormatString,
                                         time_t catchupBufferStartTime,
                                         time_t catchupBufferEndTime,
                                         long long catchupBufferOffset,
                                         int timezoneShift,
                                         int defaultProgrammeDuration,
                                         std::string& programmeCatchupId)
  : FFmpegStream(demuxPacketManager, httpProxy), m_isOpeningStream(false), m_seekOffset(0),
    m_defaultUrl(defaultUrl), m_playbackAsLive(playbackAsLive),
    m_programmeStartTime(programmeStartTime), m_programmeEndTime(programmeEndTime),
    m_catchupUrlFormatString(catchupUrlFormatString),
    m_catchupUrlNearLiveFormatString(catchupUrlNearLiveFormatString),
    m_catchupBufferStartTime(catchupBufferStartTime), m_catchupBufferEndTime(catchupBufferEndTime),
    m_catchupBufferOffset(catchupBufferOffset), m_timezoneShift(timezoneShift),
    m_defaultProgrammeDuration(defaultProgrammeDuration), m_programmeCatchupId(programmeCatchupId)
{
}

FFmpegCatchupStream::~FFmpegCatchupStream()
{

}

bool FFmpegCatchupStream::Open(const std::string& streamUrl, const std::string& mimeType, bool isRealTimeStream, const std::string& programProperty)
{
  m_isOpeningStream = true;
  bool ret = FFmpegStream::Open(streamUrl, mimeType, isRealTimeStream, programProperty);

  // We need to make an initial seek to the correct time otherwise the stream
  // will always start at the beginning instead of at the offset.
  // The value of time is irrelevant here we will want to seek to SEEK_CUR
  double temp = 0;
  DemuxSeekTime(0, false, temp);

  m_isOpeningStream = false;
  return ret;
}

bool FFmpegCatchupStream::DemuxSeekTime(double timeMs, bool backwards, double& startpts)
{
  if (/*!m_pInput ||*/ timeMs < 0)
    return false;

  int64_t seekResult = SeekCatchupStream(timeMs, m_isOpeningStream ? SEEK_CUR : SEEK_SET);
  if (seekResult >= 0)
  {
    {
      CSingleLock lock(m_critSection);
      m_seekOffset = seekResult;
    }

    Log(LOGLEVEL_DEBUG, "Seek successful. m_seekOffset = %f, m_currentPts = %f, time = %f, backwards = %d, startptr = %f",
      m_seekOffset, m_currentPts, timeMs, backwards, startpts);

    if (!m_isOpeningStream)
    {
      DemuxReset();
      return m_demuxResetOpenSuccess;
    }

    return true;
  }

  Log(LOGLEVEL_DEBUG, "Seek failed. m_currentPts = %f, time = %f, backwards = %d, startptr = %f",
    m_currentPts, timeMs, backwards, startpts);
  return false;
}

DemuxPacket* FFmpegCatchupStream::DemuxRead()
{
  DemuxPacket* pPacket = FFmpegStream::DemuxRead();
  if (pPacket)
  {
    CSingleLock lock(m_critSection);
    pPacket->pts += m_seekOffset;
    pPacket->dts += m_seekOffset;

    m_currentDemuxTime = static_cast<double>(pPacket->pts) / 1000;
  }

  return pPacket;
}

void FFmpegCatchupStream::DemuxSetSpeed(int speed)
{
  Log(LOGLEVEL_DEBUG, "DemuxSetSpeed %d", speed);

  if (IsPaused() && speed != DVD_PLAYSPEED_PAUSE)
  {
    // Resume Playback
    Log(LOGLEVEL_DEBUG, "DemuxSetSpeed - Unpause time: %lld", static_cast<long long>(m_pauseStartTime));
    double temp = 0;
    DemuxSeekTime(m_pauseStartTime, false, temp);
  }
  else if (!IsPaused() && speed == DVD_PLAYSPEED_PAUSE)
  {
    // Pause Playback
    CSingleLock lock(m_critSection);
    m_pauseStartTime = m_currentDemuxTime;
    Log(LOGLEVEL_DEBUG, "DemuxSetSpeed - Pause time: %lld", static_cast<long long>(m_pauseStartTime));
  }

  FFmpegStream::DemuxSetSpeed(speed);
}

void FFmpegCatchupStream::GetCapabilities(INPUTSTREAM_CAPABILITIES& caps)
{
  Log(LOGLEVEL_DEBUG, "GetCapabilities()");
  caps.m_mask = INPUTSTREAM_CAPABILITIES::SUPPORTS_IDEMUX |
    // INPUTSTREAM_CAPABILITIES::SUPPORTS_IDISPLAYTIME |
    INPUTSTREAM_CAPABILITIES::SUPPORTS_ITIME |
    // INPUTSTREAM_CAPABILITIES::SUPPORTS_IPOSTIME |
    INPUTSTREAM_CAPABILITIES::SUPPORTS_SEEK |
    INPUTSTREAM_CAPABILITIES::SUPPORTS_PAUSE |
    INPUTSTREAM_CAPABILITIES::SUPPORTS_ICHAPTER;
}

int64_t FFmpegCatchupStream::SeekCatchupStream(double timeMs, int whence)
{
  int64_t ret = -1;
  if (m_catchupBufferStartTime > 0)
  {
    int64_t position = static_cast<int64_t>(timeMs);
    Log(LOGLEVEL_DEBUG, "SeekCatchupStream - iPosition = %lld, iWhence = %d", position, whence);
    const time_t timeNow = time(0);
    switch (whence)
    {
      case SEEK_SET:
      {
        Log(LOGLEVEL_DEBUG, "SeekCatchupStream - SeekSet: %lld", static_cast<long long>(position));
        position += 500;
        position /= 1000;
        if (m_catchupBufferStartTime + position < timeNow - 10)
        {
          ret = position;
          m_catchupBufferOffset = position;
        }
        else
        {
          ret = timeNow - m_catchupBufferStartTime;
          m_catchupBufferOffset = ret;
        }
        ret *= DVD_TIME_BASE;

        m_streamUrl = GetUpdatedCatchupUrl();
      }
      break;
      case SEEK_CUR:
      {
        int64_t offset = m_catchupBufferOffset;
        //Log(LOGLEVEL_DEBUG, "SeekCatchupStream - timeNow = %d, startTime = %d, iTvgShift = %d, offset = %d", timeNow, m_catchupStartTime, m_programmeChannelTvgShift, offset);
        ret = offset * DVD_TIME_BASE;
      }
      break;
      default:
        Log(LOGLEVEL_DEBUG, "SeekCatchupStream - Unsupported SEEK command (%d)", whence);
      break;
    }
  }
  return ret;
}

int64_t FFmpegCatchupStream::LengthStream()
{
  int64_t length = -1;
  if (m_catchupBufferStartTime > 0 && m_catchupBufferEndTime >= m_catchupBufferStartTime)
  {
    INPUTSTREAM_TIMES times = {0};
    if (GetTimes(times) && times.ptsEnd >= times.ptsBegin)
      length = static_cast<int64_t>(times.ptsEnd - times.ptsBegin);
  }

  Log(LOGLEVEL_DEBUG, "LengthLiveStream: %lld", static_cast<long long>(length));

  return length;
}

bool FFmpegCatchupStream::GetTimes(INPUTSTREAM_TIMES& times)
{
  if (m_catchupBufferStartTime == 0)
    return false;

  times = {0};
  const time_t dateTimeNow = time(0);

  times.startTime = m_catchupBufferStartTime;
  if (m_playbackAsLive)
    times.ptsEnd = static_cast<double>(dateTimeNow - times.startTime) * DVD_TIME_BASE;
  else // it's like a video
    times.ptsEnd = static_cast<double>(std::min(dateTimeNow, m_catchupBufferEndTime) - times.startTime) * DVD_TIME_BASE;

  // Log(LOGLEVEL_DEBUG, "GetStreamTimes - Ch = %u \tTitle = \"%s\" \tepgTag->startTime = %ld \tepgTag->endTime = %ld",
  //           m_programmeUniqueChannelId, m_programmeTitle.c_str(), m_catchupBufferStartTime, m_catchupBufferEndTime);
  Log(LOGLEVEL_DEBUG, "GetStreamTimes - startTime = %ld \tptsStart = %lld \tptsBegin = %lld \tptsEnd = %lld",
            times.startTime, static_cast<long long>(times.ptsStart), static_cast<long long>(times.ptsBegin), static_cast<long long>(times.ptsEnd));

  return true;
}

void FFmpegCatchupStream::UpdateCurrentPTS()
{
  FFmpegStream::UpdateCurrentPTS();
  if (m_currentPts != DVD_NOPTS_VALUE)
    m_currentPts += m_seekOffset;
}

namespace
{

void FormatUnits(time_t tTime, const std::string& name, std::string &urlFormatString)
{
  const std::regex timeSecondsRegex(".*(\\{" + name + ":(\\d+)\\}).*");
  std::cmatch mr;
  if (std::regex_match(urlFormatString.c_str(), mr, timeSecondsRegex) && mr.length() >= 3)
  {
    std::string timeSecondsExp = mr[1].first;
    std::string second = mr[1].second;
    if (second.length() > 0)
      timeSecondsExp = timeSecondsExp.erase(timeSecondsExp.find(second));
    std::string dividerStr = mr[2].first;
    second = mr[2].second;
    if (second.length() > 0)
      dividerStr = dividerStr.erase(dividerStr.find(second));

    const time_t divider = stoi(dividerStr);
    if (divider != 0)
    {
      time_t units = tTime / divider;
      if (units < 0)
        units = 0;
      urlFormatString.replace(urlFormatString.find(timeSecondsExp), timeSecondsExp.length(), std::to_string(units));
    }
  }
}

void FormatTime(const char ch, const struct tm *pTime, std::string &urlFormatString)
{
  char str[] = { '{', ch, '}', 0 };
  auto pos = urlFormatString.find(str);
  if (pos != std::string::npos)
  {
    char buff[256], timeFmt[3];
    std::snprintf(timeFmt, sizeof(timeFmt), "%%%c", ch);
    std::strftime(buff, sizeof(buff), timeFmt, pTime);
    if (std::strlen(buff) > 0)
      urlFormatString.replace(pos, 3, buff);
  }
}

void FormatUtc(const char *str, time_t tTime, std::string &urlFormatString)
{
  auto pos = urlFormatString.find(str);
  if (pos != std::string::npos)
  {
    char buff[256];
    std::snprintf(buff, sizeof(buff), "%lu", tTime);
    urlFormatString.replace(pos, std::strlen(str), buff);
  }
}

std::string FormatDateTime(time_t dateTimeEpg, time_t duration, const std::string &urlFormatString)
{
  std::string formattedUrl = urlFormatString;

  const time_t dateTimeNow = std::time(0);
  tm* dateTime = std::localtime(&dateTimeEpg);

  FormatTime('Y', dateTime, formattedUrl);
  FormatTime('m', dateTime, formattedUrl);
  FormatTime('d', dateTime, formattedUrl);
  FormatTime('H', dateTime, formattedUrl);
  FormatTime('M', dateTime, formattedUrl);
  FormatTime('S', dateTime, formattedUrl);
  FormatUtc("{utc}", dateTimeEpg, formattedUrl);
  FormatUtc("${start}", dateTimeEpg, formattedUrl);
  FormatUtc("{utcend}", dateTimeEpg + duration, formattedUrl);
  FormatUtc("${end}", dateTimeEpg + duration, formattedUrl);
  FormatUtc("{lutc}", dateTimeNow, formattedUrl);
  FormatUtc("${timestamp}", dateTimeNow, formattedUrl);
  FormatUtc("{duration}", duration, formattedUrl);
  FormatUnits(duration, "duration", formattedUrl);
  FormatUtc("${offset}", dateTimeNow - dateTimeEpg, formattedUrl);
  FormatUnits(dateTimeNow - dateTimeEpg, "offset", formattedUrl);

  Log(LOGLEVEL_DEBUG, "CArchiveConfig::FormatDateTime - \"%s\"", formattedUrl.c_str());

  return formattedUrl;
}

} // unnamed namespace

std::string FFmpegCatchupStream::GetUpdatedCatchupUrl() const
{
  time_t timeNow = time(0);
  time_t offset = m_catchupBufferStartTime + m_catchupBufferOffset;

  if (m_catchupBufferStartTime > 0 && offset < (timeNow - 5))
  {
    time_t duration = m_defaultProgrammeDuration;
    // use the programme duration if it's valid for the offset
    if (m_programmeStartTime > 0 && m_programmeStartTime < m_programmeEndTime &&
        m_programmeStartTime <= offset && m_programmeEndTime >= offset)
      duration = m_programmeEndTime - m_programmeStartTime;

    // cap duration to timeNow
    if (offset + duration > timeNow)
      duration = timeNow - offset;

    // if we have a different URL format to use when we are close to live
    // use if we are within 4 hours of a live stream
    std::string urlFormatString = m_catchupUrlFormatString;
    if (offset > (timeNow - m_defaultProgrammeDuration) && !m_catchupUrlNearLiveFormatString.empty())
      urlFormatString = m_catchupUrlNearLiveFormatString;

    Log(LOGLEVEL_DEBUG, "Offset Time - \"%lld\" - %s", static_cast<long long>(offset), m_catchupUrlFormatString.c_str());

    std::string catchupUrl = FormatDateTime(offset - m_timezoneShift, duration, urlFormatString);

    static const std::regex CATCHUP_ID_REGEX("\\{catchup-id\\}");
    if (!m_programmeCatchupId.empty())
      catchupUrl = std::regex_replace(catchupUrl, CATCHUP_ID_REGEX, m_programmeCatchupId);

    if (!catchupUrl.empty())
    {
      Log(LOGLEVEL_DEBUG, "Catchup URL: %s", catchupUrl.c_str());
      return catchupUrl;
    }
  }

  Log(LOGLEVEL_DEBUG, "Default URL: %s", m_defaultUrl.c_str());
  return m_defaultUrl;
}