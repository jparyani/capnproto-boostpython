// Copyright (c) 2013, Kenton Varda <temporal@gmail.com>
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// 1. Redistributions of source code must retain the above copyright notice, this
//    list of conditions and the following disclaimer.
// 2. Redistributions in binary form must reproduce the above copyright notice,
//    this list of conditions and the following disclaimer in the documentation
//    and/or other materials provided with the distribution.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
// ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
// WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
// DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR
// ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
// (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
// LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
// ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
// SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#define CAPNPROTO_PRIVATE
#include "serialize-packed.h"
#include "logging.h"
#include "layout.h"
#include <vector>

namespace capnproto {

namespace internal {

PackedInputStream::PackedInputStream(BufferedInputStream& inner): inner(inner) {}
PackedInputStream::~PackedInputStream() {}

size_t PackedInputStream::read(void* dst, size_t minBytes, size_t maxBytes) {
  if (maxBytes == 0) {
    return 0;
  }

  DPRECOND(minBytes % sizeof(word) == 0, "PackedInputStream reads must be word-aligned.");
  DPRECOND(maxBytes % sizeof(word) == 0, "PackedInputStream reads must be word-aligned.");

  uint8_t* __restrict__ out = reinterpret_cast<uint8_t*>(dst);
  uint8_t* const outEnd = reinterpret_cast<uint8_t*>(dst) + maxBytes;
  uint8_t* const outMin = reinterpret_cast<uint8_t*>(dst) + minBytes;

  ArrayPtr<const byte> buffer = inner.getReadBuffer();
  VALIDATE_INPUT(buffer.size() > 0, "Premature end of packed input.") {
    return minBytes;  // garbage
  }
  const uint8_t* __restrict__ in = reinterpret_cast<const uint8_t*>(buffer.begin());

#define REFRESH_BUFFER() \
  inner.skip(buffer.size()); \
  buffer = inner.getReadBuffer(); \
  VALIDATE_INPUT(buffer.size() > 0, "Premature end of packed input.") { \
    return minBytes;  /* garbage */ \
  } \
  in = reinterpret_cast<const uint8_t*>(buffer.begin())

#define BUFFER_END (reinterpret_cast<const uint8_t*>(buffer.end()))
#define BUFFER_REMAINING ((size_t)(BUFFER_END - in))

  for (;;) {
    uint8_t tag;

    DCHECK((out - reinterpret_cast<uint8_t*>(dst)) % sizeof(word) == 0,
           "Output pointer should always be aligned here.");

    if (BUFFER_REMAINING < 10) {
      if (out >= outMin) {
        // We read at least the minimum amount, so go ahead and return.
        inner.skip(in - reinterpret_cast<const uint8_t*>(buffer.begin()));
        return out - reinterpret_cast<uint8_t*>(dst);
      }

      if (BUFFER_REMAINING == 0) {
        REFRESH_BUFFER();
        continue;
      }

      // We have at least 1, but not 10, bytes available.  We need to read slowly, doing a bounds
      // check on each byte.

      tag = *in++;

      for (uint i = 0; i < 8; i++) {
        if (tag & (1u << i)) {
          if (BUFFER_REMAINING == 0) {
            REFRESH_BUFFER();
          }
          *out++ = *in++;
        } else {
          *out++ = 0;
        }
      }

      if (BUFFER_REMAINING == 0 && (tag == 0 || tag == 0xffu)) {
        REFRESH_BUFFER();
      }
    } else {
      tag = *in++;

#define HANDLE_BYTE(n) \
      { \
         bool isNonzero = (tag & (1u << n)) != 0; \
         *out++ = *in & (-(int8_t)isNonzero); \
         in += isNonzero; \
      }

      HANDLE_BYTE(0);
      HANDLE_BYTE(1);
      HANDLE_BYTE(2);
      HANDLE_BYTE(3);
      HANDLE_BYTE(4);
      HANDLE_BYTE(5);
      HANDLE_BYTE(6);
      HANDLE_BYTE(7);
#undef HANDLE_BYTE
    }

    if (tag == 0) {
      DCHECK(BUFFER_REMAINING > 0, "Should always have non-empty buffer here.");

      uint runLength = *in++ * sizeof(word);

      VALIDATE_INPUT(runLength <= outEnd - out,
          "Packed input did not end cleanly on a segment boundary.") {
        return std::max<size_t>(minBytes, out - reinterpret_cast<uint8_t*>(dst));  // garbage
      }
      memset(out, 0, runLength);
      out += runLength;

    } else if (tag == 0xffu) {
      DCHECK(BUFFER_REMAINING > 0, "Should always have non-empty buffer here.");

      uint runLength = *in++ * sizeof(word);

      VALIDATE_INPUT(runLength <= outEnd - out,
          "Packed input did not end cleanly on a segment boundary.") {
        return std::max<size_t>(minBytes, out - reinterpret_cast<uint8_t*>(dst));  // garbage
      }

      uint inRemaining = BUFFER_REMAINING;
      if (inRemaining >= runLength) {
        // Fast path.
        memcpy(out, in, runLength);
        out += runLength;
        in += runLength;
      } else {
        // Copy over the first buffer, then do one big read for the rest.
        memcpy(out, in, inRemaining);
        out += inRemaining;
        runLength -= inRemaining;

        inner.skip(buffer.size());
        inner.read(out, runLength);
        out += runLength;

        if (out == outEnd) {
          return maxBytes;
        } else {
          buffer = inner.getReadBuffer();
          in = reinterpret_cast<const uint8_t*>(buffer.begin());

          // Skip the bounds check below since we just did the same check above.
          continue;
        }
      }
    }

    if (out == outEnd) {
      inner.skip(in - reinterpret_cast<const uint8_t*>(buffer.begin()));
      return maxBytes;
    }
  }

  FAIL_CHECK("Can't get here.");
  return 0;  // GCC knows FAIL_CHECK doesn't return, but Eclipse CDT still warns...

#undef REFRESH_BUFFER
}

void PackedInputStream::skip(size_t bytes) {
  // We can't just read into buffers because buffers must end on block boundaries.

  if (bytes == 0) {
    return;
  }

  DPRECOND(bytes % sizeof(word) == 0, "PackedInputStream reads must be word-aligned.");

  ArrayPtr<const byte> buffer = inner.getReadBuffer();
  const uint8_t* __restrict__ in = reinterpret_cast<const uint8_t*>(buffer.begin());

#define REFRESH_BUFFER() \
  inner.skip(buffer.size()); \
  buffer = inner.getReadBuffer(); \
  VALIDATE_INPUT(buffer.size() > 0, "Premature end of packed input.") return; \
  in = reinterpret_cast<const uint8_t*>(buffer.begin())

  for (;;) {
    uint8_t tag;

    if (BUFFER_REMAINING < 10) {
      if (BUFFER_REMAINING == 0) {
        REFRESH_BUFFER();
        continue;
      }

      // We have at least 1, but not 10, bytes available.  We need to read slowly, doing a bounds
      // check on each byte.

      tag = *in++;

      for (uint i = 0; i < 8; i++) {
        if (tag & (1u << i)) {
          if (BUFFER_REMAINING == 0) {
            REFRESH_BUFFER();
          }
          in++;
        }
      }
      bytes -= 8;

      if (BUFFER_REMAINING == 0 && (tag == 0 || tag == 0xffu)) {
        REFRESH_BUFFER();
      }
    } else {
      tag = *in++;

#define HANDLE_BYTE(n) \
      in += (tag & (1u << n)) != 0

      HANDLE_BYTE(0);
      HANDLE_BYTE(1);
      HANDLE_BYTE(2);
      HANDLE_BYTE(3);
      HANDLE_BYTE(4);
      HANDLE_BYTE(5);
      HANDLE_BYTE(6);
      HANDLE_BYTE(7);
#undef HANDLE_BYTE

      bytes -= 8;
    }

    if (tag == 0) {
      DCHECK(BUFFER_REMAINING > 0, "Should always have non-empty buffer here.");

      uint runLength = *in++ * sizeof(word);

      VALIDATE_INPUT(runLength <= bytes,
          "Packed input did not end cleanly on a segment boundary.") {
        return;
      }

      bytes -= runLength;

    } else if (tag == 0xffu) {
      DCHECK(BUFFER_REMAINING > 0, "Should always have non-empty buffer here.");

      uint runLength = *in++ * sizeof(word);

      VALIDATE_INPUT(runLength <= bytes,
          "Packed input did not end cleanly on a segment boundary.") {
        return;
      }

      bytes -= runLength;

      uint inRemaining = BUFFER_REMAINING;
      if (inRemaining > runLength) {
        // Fast path.
        in += runLength;
      } else {
        // Forward skip to the underlying stream.
        runLength -= inRemaining;
        inner.skip(buffer.size() + runLength);

        if (bytes == 0) {
          return;
        } else {
          buffer = inner.getReadBuffer();
          in = reinterpret_cast<const uint8_t*>(buffer.begin());

          // Skip the bounds check below since we just did the same check above.
          continue;
        }
      }
    }

    if (bytes == 0) {
      inner.skip(in - reinterpret_cast<const uint8_t*>(buffer.begin()));
      return;
    }
  }

  FAIL_CHECK("Can't get here.");
}

// -------------------------------------------------------------------

PackedOutputStream::PackedOutputStream(BufferedOutputStream& inner)
    : inner(inner) {}
PackedOutputStream::~PackedOutputStream() {}

void PackedOutputStream::write(const void* src, size_t size) {
  ArrayPtr<byte> buffer = inner.getWriteBuffer();
  byte slowBuffer[20];

  uint8_t* __restrict__ out = reinterpret_cast<uint8_t*>(buffer.begin());

  const uint8_t* __restrict__ in = reinterpret_cast<const uint8_t*>(src);
  const uint8_t* const inEnd = reinterpret_cast<const uint8_t*>(src) + size;

  while (in < inEnd) {
    if (reinterpret_cast<uint8_t*>(buffer.end()) - out < 10) {
      // Oops, we're out of space.  We need at least 10 bytes for the fast path, since we don't
      // bounds-check on every byte.

      // Write what we have so far.
      inner.write(buffer.begin(), out - reinterpret_cast<uint8_t*>(buffer.begin()));

      // Use a slow buffer into which we'll encode 10 to 20 bytes.  This should get us past the
      // output stream's buffer boundary.
      buffer = arrayPtr(slowBuffer, sizeof(slowBuffer));
      out = reinterpret_cast<uint8_t*>(buffer.begin());
    }

    uint8_t* tagPos = out++;

#define HANDLE_BYTE(n) \
    uint8_t bit##n = *in != 0; \
    *out = *in; \
    out += bit##n; /* out only advances if the byte was non-zero */ \
    ++in

    HANDLE_BYTE(0);
    HANDLE_BYTE(1);
    HANDLE_BYTE(2);
    HANDLE_BYTE(3);
    HANDLE_BYTE(4);
    HANDLE_BYTE(5);
    HANDLE_BYTE(6);
    HANDLE_BYTE(7);
#undef HANDLE_BYTE

    uint8_t tag = (bit0 << 0) | (bit1 << 1) | (bit2 << 2) | (bit3 << 3)
                | (bit4 << 4) | (bit5 << 5) | (bit6 << 6) | (bit7 << 7);
    *tagPos = tag;

    if (tag == 0) {
      // An all-zero word is followed by a count of consecutive zero words (not including the
      // first one).

      // We can check a whole word at a time.
      const uint64_t* inWord = reinterpret_cast<const uint64_t*>(in);

      // The count must fit it 1 byte, so limit to 255 words.
      const uint64_t* limit = reinterpret_cast<const uint64_t*>(inEnd);
      if (limit - inWord > 255) {
        limit = inWord + 255;
      }

      while (inWord < limit && *inWord == 0) {
        ++inWord;
      }

      // Write the count.
      *out++ = inWord - reinterpret_cast<const uint64_t*>(in);

      // Advance input.
      in = reinterpret_cast<const uint8_t*>(inWord);

    } else if (tag == 0xffu) {
      // An all-nonzero word is followed by a count of consecutive uncompressed words, followed
      // by the uncompressed words themselves.

      // Count the number of consecutive words in the input which have no more than a single
      // zero-byte.  We look for at least two zeros because that's the point where our compression
      // scheme becomes a net win.
      // TODO(perf):  Maybe look for three zeros?  Compressing a two-zero word is a loss if the
      //   following word has no zeros.
      const uint8_t* runStart = in;

      const uint8_t* limit = inEnd;
      if ((size_t)(limit - in) > 255 * sizeof(word)) {
        limit = in + 255 * sizeof(word);
      }

      while (in < limit) {
        // Check eight input bytes for zeros.
        uint c = *in++ == 0;
        c += *in++ == 0;
        c += *in++ == 0;
        c += *in++ == 0;
        c += *in++ == 0;
        c += *in++ == 0;
        c += *in++ == 0;
        c += *in++ == 0;

        if (c >= 2) {
          // Un-read the word with multiple zeros, since we'll want to compress that one.
          in -= 8;
          break;
        }
      }

      // Write the count.
      uint count = in - runStart;
      *out++ = count / sizeof(word);

      if (count <= reinterpret_cast<uint8_t*>(buffer.end()) - out) {
        // There's enough space to memcpy.
        memcpy(out, runStart, count);
        out += count;
      } else {
        // Input overruns the output buffer.  We'll give it to the output stream in one chunk
        // and let it decide what to do.
        inner.write(buffer.begin(), reinterpret_cast<byte*>(out) - buffer.begin());
        inner.write(runStart, in - runStart);
        buffer = inner.getWriteBuffer();
        out = reinterpret_cast<uint8_t*>(buffer.begin());
      }
    }
  }

  // Write whatever is left.
  inner.write(buffer.begin(), reinterpret_cast<byte*>(out) - buffer.begin());
}

}  // namespace internal

// =======================================================================================

PackedMessageReader::PackedMessageReader(
    BufferedInputStream& inputStream, ReaderOptions options, ArrayPtr<word> scratchSpace)
    : PackedInputStream(inputStream),
      InputStreamMessageReader(static_cast<PackedInputStream&>(*this), options, scratchSpace) {}

PackedMessageReader::~PackedMessageReader() {}

PackedFdMessageReader::PackedFdMessageReader(
    int fd, ReaderOptions options, ArrayPtr<word> scratchSpace)
    : FdInputStream(fd),
      BufferedInputStreamWrapper(static_cast<FdInputStream&>(*this)),
      PackedMessageReader(static_cast<BufferedInputStreamWrapper&>(*this),
                          options, scratchSpace) {}

PackedFdMessageReader::PackedFdMessageReader(
    AutoCloseFd fd, ReaderOptions options, ArrayPtr<word> scratchSpace)
    : FdInputStream(move(fd)),
      BufferedInputStreamWrapper(static_cast<FdInputStream&>(*this)),
      PackedMessageReader(static_cast<BufferedInputStreamWrapper&>(*this),
                          options, scratchSpace) {}

PackedFdMessageReader::~PackedFdMessageReader() {}

void writePackedMessage(BufferedOutputStream& output,
                        ArrayPtr<const ArrayPtr<const word>> segments) {
  internal::PackedOutputStream packedOutput(output);
  writeMessage(packedOutput, segments);
}

void writePackedMessage(OutputStream& output, ArrayPtr<const ArrayPtr<const word>> segments) {
  if (BufferedOutputStream* bufferedOutputPtr = dynamic_cast<BufferedOutputStream*>(&output)) {
    writePackedMessage(*bufferedOutputPtr, segments);
  } else {
    byte buffer[8192];
    BufferedOutputStreamWrapper bufferedOutput(output, arrayPtr(buffer, sizeof(buffer)));
    writePackedMessage(bufferedOutput, segments);
  }
}

void writePackedMessageToFd(int fd, ArrayPtr<const ArrayPtr<const word>> segments) {
  FdOutputStream output(fd);
  writePackedMessage(output, segments);
}

}  // namespace capnproto
