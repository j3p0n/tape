/*
 * Copyright (C) 2012 Square, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h> // for fsync()

#include "fileio.h"
#include "logutil.h"

// copy buffer is on stack.
#define COPY_BUFFER_SIZE 4096

// sanity limit of 2GB
#define FILE_HARD_SANITY_LIMIT (1<<31)

/**
 * File utility primitives somewhat patterned on RandomAccessFile.
 *
 * @author Jochen Bekmann (jochen@squareup.com)
 */

bool FileIo_seek(FILE* file, uint32_t position) {
  if (position > (uint32_t)FILE_HARD_SANITY_LIMIT) {
    LOG(LFATAL, "Requested seek (%d) exceeds sanity hard limit %d", position,
        FILE_HARD_SANITY_LIMIT);
    return false;
  }

  if (fseek(file, (long)position, SEEK_SET) < 0) {
    LOG(LWARN, "Error setting file position to %d. fhandle %d", position,
        fileno(file));
    return false;
  }
  return true;
}

bool FileIo_write(FILE* file, const byte* buffer, uint32_t buffer_offset,
                      uint32_t length) {
  if (length > (uint32_t)FILE_HARD_SANITY_LIMIT || buffer_offset > (uint32_t)FILE_HARD_SANITY_LIMIT) {
    LOG(LFATAL, "Requested file write %d or offset %d exceeds sanity hard limit %d",
        length, buffer_offset, FILE_HARD_SANITY_LIMIT);
    return false;
  }
  if (fwrite(buffer + buffer_offset, (size_t)1, (size_t)length, file) != length) {
    LOG(LWARN, "Error writing data, fhandle %d", fileno(file));
    return false;
  }
  if (fflush(file) != 0 || fsync(fileno(file)) != 0) {
    LOG(LWARN, "Error flushing file, fhandle %d", fileno(file));
    return false;
  }
  return true;
}

bool FileIo_read(FILE* file, void* buffer, uint32_t buffer_offset, uint32_t length) {
  if (length > (uint32_t)FILE_HARD_SANITY_LIMIT) {
    LOG(LFATAL, "Requested seek (%d) exceeds sanity hard limit %d", length,
        FILE_HARD_SANITY_LIMIT);
    return false;
  }
  if (fread(buffer + buffer_offset, (size_t)1, (size_t)length, file) != length) {
    LOG(LWARN, "Error reading element from fhandle %d", fileno(file));
    return false;
  }
  return true;
}

off_t FileIo_getLength(FILE* file) {
  struct stat filestat;
  if (fstat(fileno(file), &filestat) != 0) {
    LOG(LWARN, "Error getting file stat. fhandle %d", fileno(file));
    return -1;
  }
  return filestat.st_size;
}

// Length must be multiple of 4.
bool FileIo_writeZeros(FILE* file, uint32_t length) {
  if (length % sizeof(uint32_t) != 0) {
    LOG(LFATAL, "Initial file length must be multiple of 4 bytes, got %d", length);
    return false;
  }

  byte buf[] = {0,0,0,0};
  uint32_t c;
  for (c = 0; c <= length / 4; c++) {
    if (fwrite(buf, sizeof(buf), (size_t)1, file) != 1) {
      return false;
    }
  }

  if (fflush(file) != 0 || fsync(fileno(file)) != 0) {
    LOG(LWARN, "Error flushing file, fhandle %d", fileno(file));
    return false;
  }
  return true;
}

/** Some systems allow the file length to be adjusted using truncate, as
 * some JVMs do. */
bool FileIo_setLength(FILE* file, uint32_t length) {
  if (length > (uint32_t)FILE_HARD_SANITY_LIMIT) {
    LOG(LFATAL, "Requested file size (%d) exceeds sanity hard limit %d", length,
        FILE_HARD_SANITY_LIMIT);
    return false;
  }
  if (ftruncate(fileno(file), (off_t)length) != 0 || fsync(fileno(file)) != 0) {
    LOG(LWARN, "Error setting file length to %d, fhandle %d", length,
        fileno(file));
    return false;
  }
  return true;
}


/** Will copy part of a file to another offset, the caller is responsible for
 * checking that there is enough data from the source to cover length.
 * The parts to transfer may not overlap.
 * TODO: if needed, overlap handling to be more accommodating.
 * TODO: investigate whether fread and fwrite make efficient use of the
 *       FILE's read cache. Use sendfile for Android, investigate for OSX.
 **/
bool FileIo_transferTo(FILE *file, uint32_t source, uint32_t destination,
    uint32_t length) {
  byte buffer[COPY_BUFFER_SIZE];

  if ((destination > source && source + length > destination) ||
      (destination < source && destination + length > source)){
    LOG(LWARN, "Can't transfer between overlapping parts of file. "
        "src=%d dest=%d len=%d, fhandle %d",
        source, destination, length, fileno(file));
    return false;
  }

  while (length > 0) {
    if (!FileIo_seek(file, source)) return false;

    uint32_t copylen = length < COPY_BUFFER_SIZE ? length : COPY_BUFFER_SIZE;
    size_t read = fread(buffer, (size_t)1, (size_t)copylen, file);
    if (read < copylen) {
      LOG(LWARN, "Error reading file, src=%d dest=%d len=%d (%d), fhandle %d",
          source, destination, length, copylen, fileno(file));
      return false;
    }
    if (!FileIo_seek(file, destination)) return false;
    size_t wrote = fwrite(buffer, (size_t)1, read, file);
    if (wrote < read) {
      LOG(LWARN, "Error writing file, src=%d dest=%d len=%d (%d), fhandle %d",
          source, destination, length, read, fileno(file));
      return false;
    }
    length -= wrote;
    source += wrote;
    destination += wrote;
  }
  if (fflush(file) != 0 || fsync(fileno(file)) != 0) {
    LOG(LWARN, "Error flushing file, fhandle %d", fileno(file));
    return false;
  }
  return true;
}







