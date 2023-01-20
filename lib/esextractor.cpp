/* ESExtractor
 * Copyright (C) 2022 Igalia, S.L.
 *     Author: Stephane Cerveau <scerveau@igalia.com>
 *
 * Licensed under the Apache License, Version 2.0 (the "License"); you
 * may not use this file except in compliance with the License.  You
 * may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or
 * implied.  See the License for the specific language governing
 * permissions and limitations under the License.
 */

#include "esextractor.h"


#define H26X_MAX_PROBE_LENGTH (128 * 1024)
#define MPEG_HEADER_SIZE 3
#define MAX_SEARCH_SIZE 5

const int NAL_UNIT_TYPE_MASK = 0x1F;

template < typename T > static
    std::vector <
    T >
subVector (std::vector < T > const &v, int pos, int size)
{
  auto first = v.cbegin () + pos;
  auto last = v.cbegin () + pos + size + 1;

  std::vector < T > vec (first, last);
  return vec;
}

inline bool
isMPEGHeader (std::vector < unsigned char >buffer)
{
  if (buffer.size () < MPEG_HEADER_SIZE)
    return false;
  bool
      found = (buffer[0] == 0x00 && buffer[1] == 0x00 && buffer[2] == 0x01);
  if (found) {
    DBG ("Found MPEG header");
  }
  return found;
}

int32_t
scanMPEGHeader (std::vector < unsigned char >buffer, int32_t pos)
{
  for (uint i = pos; i < buffer.size (); i++) {
    bool
        found = (buffer[i] == 0x00 && buffer[i + 1] == 0x00
        && buffer[i + 2] == 0x01);
    if (found)
      return i;
  }
  return -1;
}

ESExtractor::ESExtractor ()
{
  reset ();
}

ESExtractor::~ESExtractor ()
{
  DBG ("Found %u frame and read %d of %d", m_frameCount, m_readSize,
      m_fileSize);
}

void
ESExtractor::reset ()
{
  m_filePosition = 0;
  m_fileSize = 0;
  m_bufferPosition = 0;
  m_frameStartPos = 0;
  m_mpeg_detected = false;
  m_codec = ES_EXTRACTOR_VIDEO_CODEC_UNKNOWN;
  m_frameState = ES_EXTRACTOR_FRAME_STATE_NONE;
  m_frameCount = 0;
  m_readSize = 0;
  m_eos = false;
  m_buffer = std::vector < unsigned char >();
  m_nextFrame = std::vector < unsigned char >();
}

bool
ESExtractor::openFile (const char *fileName)
{
  if (m_file.is_open ())
    return true;
  m_file = std::ifstream (fileName, std::ios::binary | std::ios::ate);
  return m_file.is_open ();
}

std::vector < unsigned char >
ESExtractor::readFile (int32_t data_size, int32_t pos, bool append)
{
  if (!m_fileSize)
    m_fileSize = m_file.tellg ();

  DBG ("Read %ld at pos %ld append %d", data_size, pos, append);
  m_file.seekg (pos, std::ios::beg);
  std::vector < unsigned char >buffer;
  buffer.resize (data_size);
  m_file.read ((char *) buffer.data (), data_size);
  size_t read_size = m_file.gcount ();
  buffer.resize (read_size);
  m_readSize += read_size;
  if (append) {
    m_buffer.insert (m_buffer.end (), buffer.begin (), buffer.end ());
    DBG ("ReadFile: Append %d to a buffer of new size %ld read %ld", data_size,
        m_buffer.size (), read_size);
  } else {
    m_buffer = buffer;
    DBG ("ReadFile: Read buffer %d of size read %ld", data_size, read_size);
  }
  return m_buffer;
}


std::vector < unsigned char >
ESExtractor::prepareFrame (std::vector < unsigned char >buffer, uint start,
    uint end)
{
  int frame_start = start;
  if (start > buffer.size () || end > buffer.size ()) {
    throw
        std::out_of_range
        ("start and end positions must be within the buffer size");
  }
  if (start > end) {
    throw
        std::invalid_argument ("start position must be less than end position");
  }
  std::vector < unsigned char >frame =
      std::vector < unsigned char >(buffer.begin () + frame_start,
      buffer.begin () + end);
  std::vector < unsigned char >start_code = { 0x00, 0x00, 0x00, 0x01 };
  frame.insert (frame.begin (), start_code.begin (), start_code.end ());
  return frame;
}

void
ESExtractor::printBufferHex (std::vector < unsigned char >buffer)
{
for (unsigned char data:buffer) {
    std::cout << std::hex << (int) data << " ";
  }
  std::cout << std::endl;
}

bool
ESExtractor::isH265 (std::vector < unsigned char >buffer)
{
  int nut;
  bool found = false;

  if (buffer.size () < MAX_SEARCH_SIZE)
    return false;

  /* forbiden_zero_bit | nal_unit_type */
  nut = buffer[0] & 0xfe;

  /* if forbidden bit is different to 0 won't be h265 */
  if (nut > 0x7e) {
    found = false;
  }
  nut = nut >> 1;

  /* if nuh_layer_id is not zero or nuh_temporal_id_plus1 is zero then
   * it won't be h265 */
  if ((buffer[3] & 0x01) || (buffer[4] & 0xf8) || !(buffer[4] & 0x07)) {
    found = false;
  }
  if ((nut >= 0 && nut <= 9) || (nut >= 16 && nut <= 21) || (nut >= 32
          && nut <= 40)) {
    found = true;
  }

  if (found) {
    m_codec = ES_EXTRACTOR_VIDEO_CODEC_H265;
    DBG ("Found h265");
  }
  return found;
}

bool
ESExtractor::isH264 (std::vector < unsigned char >buffer)
{
  int nut, ref;
  bool found = false;

  if (buffer.size () < MAX_SEARCH_SIZE)
    return found;

  nut = buffer[0] & 0x9f;       /* forbiden_zero_bit | nal_unit_type */
  ref = buffer[0] & 0x60;       /* nal_ref_idc */

  /* if forbidden bit is different to 0 won't be h264 */
  if (nut > 0x1f) {
    found = false;
  }

  if ((nut >= 1 && nut <= 13) || nut == 19) {
    if ((nut == 5 && ref == 0) ||
        ((nut == 6 || (nut >= 9 && nut <= 12)) && ref != 0)) {
      found = false;
    } else {
      found = true;
    }
  } else if (nut >= 14 && nut <= 33) {
    if (nut == 14 || nut == 15 || nut == 20) {
      found = true;
    }
  }

  if (found) {
    m_codec = ES_EXTRACTOR_VIDEO_CODEC_H264;
    DBG ("Found h264");
  }

  return found;
}

int32_t ESExtractor::parseStream (int32_t start_position)
{

  int32_t
      pos = start_position;
  int32_t
      buffer_size = m_buffer.size ();
  int32_t
      offset = 0;

  while (pos < buffer_size) {
    if (!m_mpeg_detected) {
      offset = scanMPEGHeader (m_buffer, pos);
      if (offset > 0) {
        /* start code might have 2 or 3 0-bytes */
        pos += 3 + offset;
        std::vector < unsigned char >
            buffer = subVector (m_buffer, pos, MAX_SEARCH_SIZE);
        if (isH264 (buffer) || isH265 (buffer)) {
          m_mpeg_detected = true;
          m_frameStartPos = pos;
          m_frameState = ES_EXTRACTOR_FRAME_STATE_START;
        } else {
          ERR ("Found a MPEG but no valid codec");
          return -1;
          m_frameStartPos = pos;
        }
      }
    } else if (m_mpeg_detected && m_codec != ES_EXTRACTOR_VIDEO_CODEC_UNKNOWN) {
      pos = scanMPEGHeader (m_buffer, pos);
      if (pos >= 0) {
        DBG ("Found a NAL delimiter, stop pos %ld ", pos);
        if (m_frameState == ES_EXTRACTOR_FRAME_STATE_NONE) {
          m_frameState = ES_EXTRACTOR_FRAME_STATE_START;
          pos += 3;
          m_frameStartPos = pos;
        } else {
          m_frameState = ES_EXTRACTOR_FRAME_STATE_END;
          if (m_buffer[pos - 1] == 0x00)
            pos -= 1;
        }
        return pos;
      } else {
        DBG ("Unable to find a new MPEG code, end of file");
        pos = buffer_size;
      }
    }
  }

  return pos;
}

ESExtractorResult
ESExtractor::readStream ()
{
  int32_t pos;

  if (m_eos) {
    m_nextFrame = std::vector < unsigned char >();
    return ES_EXTRACTOR_RESULT_EOS;
  }
  if (m_bufferPosition >= (int32_t) m_buffer.size ())
    readFile (H26X_MAX_PROBE_LENGTH, m_filePosition, false);

  while (m_filePosition < m_fileSize) {
    if (m_bufferPosition <= (int32_t) m_buffer.size ()) {
      pos = parseStream (m_bufferPosition);
      if (pos == (int32_t) - 1) {
        return ES_EXTRACTOR_RESULT_NO_PACKET;
      } else {
        if (m_frameState == ES_EXTRACTOR_FRAME_STATE_END) {
          m_nextFrame = prepareFrame (m_buffer, m_frameStartPos, pos);
          m_frameCount++;
          DBG ("Found a new frame (%d) of size %ld at pos %d", m_frameCount,
              m_nextFrame.size (), m_filePosition + m_frameStartPos);
          m_bufferPosition = m_frameStartPos = pos;
          m_frameState = ES_EXTRACTOR_FRAME_STATE_NONE;
          if (m_buffer.size () > H26X_MAX_PROBE_LENGTH) {
            m_filePosition += pos;
            readFile (H26X_MAX_PROBE_LENGTH, m_filePosition, false);
            m_bufferPosition = m_frameStartPos = 0;
          }
          return ES_EXTRACTOR_RESULT_NEW_PACKET;
        } else {
          m_bufferPosition = pos;
          if (m_bufferPosition >= (int32_t) m_buffer.size ()) {
            if (m_filePosition + m_bufferPosition >= m_fileSize) {

              m_nextFrame =
                  prepareFrame (m_buffer, m_frameStartPos, m_buffer.size ());
              m_frameCount++;
              DBG ("Found a last frame (%d) of size %ld at pos %d",
                  m_frameCount, m_nextFrame.size (),
                  m_filePosition + m_frameStartPos);
              m_eos = true;
              return ES_EXTRACTOR_RESULT_LAST_PACKET;
            } else
              readFile (H26X_MAX_PROBE_LENGTH,
                  m_filePosition + m_buffer.size (), true);
          }
        }
      }
    }
  }
  return ES_EXTRACTOR_RESULT_NO_PACKET;
}

ESExtractor *
es_extractor_new (const char *uri)
{
  ESExtractor *extractor = new ESExtractor ();
  if (extractor->openFile (uri)) {
    extractor->readStream ();
    if (extractor->getCodec () > ES_EXTRACTOR_VIDEO_CODEC_UNKNOWN) {
      return extractor;
    }
  }

  es_extractor_teardown (extractor);
  return NULL;
}

ESExtractorResult
es_extractor_read_frame (ESExtractor * extractor, uint8_t ** packet, int *size)
{
  ESExtractorResult res;
  std::vector < unsigned char >*nextFrame = extractor->getNextFrame ();
  if (nextFrame->size () == 0)
    res = ES_EXTRACTOR_RESULT_EOS;
  *packet = nextFrame->data ();
  *size = nextFrame->size ();
  res = extractor->readStream ();
  return res;
}

ESExtractorVideoCodec
es_extractor_video_codec (ESExtractor * extractor)
{
  return extractor->getCodec ();
}

int
es_extractor_frame_count (ESExtractor * extractor)
{
  return extractor->frameCount ();
}

void
es_extractor_teardown (ESExtractor * extractor)
{
  delete (extractor);
}
