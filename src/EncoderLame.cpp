/*
 *  Copyright (C) 2005-2021 Team Kodi (https://kodi.tv)
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSE.md for more information.
 */

#include <algorithm>
#include <iconv.h>
#include <kodi/addon-instance/AudioEncoder.h>
#include <lame/lame.h>
#include <stdlib.h>
#include <string.h>

class ATTR_DLL_LOCAL CEncoderLame : public kodi::addon::CInstanceAudioEncoder
{
public:
  CEncoderLame(KODI_HANDLE instance, const std::string& version);
  ~CEncoderLame() override;

  bool Start(const kodi::addon::AudioEncoderInfoTag& tag) override;
  ssize_t Encode(const uint8_t* stream, size_t numBytesRead) override;
  bool Finish() override;

private:
  short unsigned int* ToUtf16(const char* src);

  lame_global_flags* m_encoder; ///< lame encoder context
  int m_audio_pos; ///< audio position in file
  uint8_t m_buffer[65536]; ///< buffer for writing out audio data
  int m_preset;
  int m_bitrate;
};


CEncoderLame::CEncoderLame(KODI_HANDLE instance, const std::string& version)
  : CInstanceAudioEncoder(instance, version), m_audio_pos(0), m_preset(-1)
{
  m_encoder = lame_init();
  if (!m_encoder)
  {
    kodi::Log(ADDON_LOG_ERROR, "Failed to construct lame stream encoder");
    return;
  }

  int value = kodi::GetSettingInt("preset");
  if (value == 0)
    m_preset = MEDIUM;
  else if (value == 1)
    m_preset = STANDARD;
  else if (value == 2)
    m_preset = EXTREME;

  m_bitrate = 128 + 32 * kodi::GetSettingInt("bitrate");

  if (m_preset == -1)
    lame_set_brate(m_encoder, m_bitrate);
  else
    lame_set_preset(m_encoder, m_preset);

  lame_set_asm_optimizations(m_encoder, MMX, 1);
  lame_set_asm_optimizations(m_encoder, SSE, 1);
}

CEncoderLame::~CEncoderLame()
{
  lame_close(m_encoder);
}

bool CEncoderLame::Start(const kodi::addon::AudioEncoderInfoTag& tag)
{
  if (!m_encoder)
    return false;

  // we accept only 2 ch 16 bit atm
  if (tag.GetChannels() != 2 || tag.GetBitsPerSample() != 16)
  {
    kodi::Log(ADDON_LOG_ERROR, "Invalid input format to encode");
    return false;
  }

  lame_set_in_samplerate(m_encoder, tag.GetSamplerate());

  // disable automatic ID3 tag writing - we'll write ourselves
  lame_set_write_id3tag_automatic(m_encoder, 0);

  // Setup the ID3 tagger
  id3tag_init(m_encoder);
  id3tag_set_title(m_encoder, tag.GetTitle().c_str());
  id3tag_set_artist(m_encoder, tag.GetArtist().c_str());
  id3tag_set_album(m_encoder, tag.GetAlbum().c_str());
  id3tag_set_year(m_encoder, tag.GetReleaseDate().c_str());
  id3tag_set_track(m_encoder, std::to_string(tag.GetTrack()).c_str());
  int test = id3tag_set_genre(m_encoder, tag.GetGenre().c_str());
  if (test == -1)
    id3tag_set_genre(m_encoder, "Other");

  bool useVer2 = kodi::GetSettingInt("id3version") == 2;
  if (useVer2)
  {
    id3tag_add_v2(m_encoder);

    short unsigned int* value;

    value = ToUtf16(tag.GetArtist().c_str());
    id3tag_set_textinfo_utf16(m_encoder, "TPE1", value);
    free(value);

    value = ToUtf16(tag.GetTitle().c_str());
    id3tag_set_textinfo_utf16(m_encoder, "TIT2", value);
    free(value);

    value = ToUtf16(tag.GetArtist().c_str());
    id3tag_set_textinfo_utf16(m_encoder, "TPE1", value);
    free(value);

    value = ToUtf16(tag.GetAlbumArtist().c_str());
    id3tag_set_textinfo_utf16(m_encoder, "TPE2", value);
    free(value);

    value = ToUtf16(tag.GetAlbum().c_str());
    id3tag_set_textinfo_utf16(m_encoder, "TALB", value);
    free(value);

    value = ToUtf16(tag.GetReleaseDate().c_str());
    id3tag_set_textinfo_utf16(m_encoder, "TYER", value);
    free(value);

    value = ToUtf16(std::to_string(tag.GetTrack()).c_str());
    id3tag_set_textinfo_utf16(m_encoder, "TRCK", value);
    free(value);

    value = ToUtf16(tag.GetGenre().c_str());
    id3tag_set_textinfo_utf16(m_encoder, "TCON", value);
    free(value);

    value = ToUtf16(tag.GetComment().c_str());
    id3tag_set_comment_utf16(m_encoder, 0, 0, value);
    free(value);
  }

  // Now that all the options are set, lame needs to analyze them and
  // set some more internal options and check for problems
  if (lame_init_params(m_encoder) < 0)
  {
    return false;
  }

  // now write the ID3 tag information, storing the position
  int tag_length;
  if (useVer2)
    tag_length = lame_get_id3v2_tag(m_encoder, m_buffer, sizeof(m_buffer));
  else
    tag_length = lame_get_id3v1_tag(m_encoder, m_buffer, sizeof(m_buffer));
  if (tag_length)
  {
    Write(m_buffer, tag_length);
    m_audio_pos = tag_length;
  }

  return true;
}

ssize_t CEncoderLame::Encode(const uint8_t* stream, size_t numBytesRead)
{
  if (!m_encoder)
    return -1;

  // note: assumes 2ch 16bit atm
  const size_t bytes_per_frame = 2 * 2;

  size_t bytes_left = numBytesRead;
  while (bytes_left)
  {
    const size_t frames = std::min(bytes_left / bytes_per_frame, size_t(4096));

    int written = lame_encode_buffer_interleaved(m_encoder, (short*)stream, frames, m_buffer,
                                                 sizeof(m_buffer));
    if (written < 0)
      return -1; // error
    Write(m_buffer, written);

    stream += frames * bytes_per_frame;
    bytes_left -= frames * bytes_per_frame;
  }

  return numBytesRead - bytes_left;
}

bool CEncoderLame::Finish()
{
  if (!m_encoder)
    return false;

  // may return one more mp3 frames
  int written = lame_encode_flush(m_encoder, m_buffer, sizeof(m_buffer));
  if (written < 0)
    return false;

  Write(m_buffer, written);

  // write id3v1 tag to file
  int id3v1tag = lame_get_id3v1_tag(m_encoder, m_buffer, sizeof(m_buffer));
  if (id3v1tag > 0)
    Write(m_buffer, id3v1tag);

  // update LAME/Xing tag
  int lameTag = lame_get_lametag_frame(m_encoder, m_buffer, sizeof(m_buffer));
  if (m_audio_pos && lameTag > 0)
  {
    Seek(m_audio_pos, SEEK_SET);
    Write(m_buffer, lameTag);
  }

  return true;
}

short unsigned int* CEncoderLame::ToUtf16(const char* src)
{
  short unsigned int* dst = 0;
  if (src != nullptr)
  {
    size_t const l = strlen(src);
    size_t const n = (l + 1) * 4;
    dst = static_cast<short unsigned int*>(calloc(n + 4, 4));
    if (dst != 0)
    {
      iconv_t xiconv = iconv_open("UTF-16LE//TRANSLIT", "UTF-8");
      dst[0] = 0xfeff; /* BOM */
      if (xiconv != (iconv_t)-1)
      {
#ifndef _WIN32
        char* i_ptr = const_cast<char*>(src);
#else
        const char* i_ptr = src;
#endif
        short unsigned int* o_ptr = &dst[1];
        size_t srcln = l;
        size_t avail = n;
        iconv(xiconv, &i_ptr, &srcln, reinterpret_cast<char**>(&o_ptr), &avail);
        iconv_close(xiconv);
      }
    }
  }
  return dst;
}

//------------------------------------------------------------------------------

class ATTR_DLL_LOCAL CMyAddon : public kodi::addon::CAddonBase
{
public:
  CMyAddon() = default;
  ADDON_STATUS CreateInstance(int instanceType,
                              const std::string& instanceID,
                              KODI_HANDLE instance,
                              const std::string& version,
                              KODI_HANDLE& addonInstance) override;
};

ADDON_STATUS CMyAddon::CreateInstance(int instanceType,
                                      const std::string& instanceID,
                                      KODI_HANDLE instance,
                                      const std::string& version,
                                      KODI_HANDLE& addonInstance)
{
  addonInstance = new CEncoderLame(instance, version);
  return ADDON_STATUS_OK;
}

ADDONCREATOR(CMyAddon)
