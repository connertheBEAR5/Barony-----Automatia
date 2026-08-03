#pragma once

/*-------------------------------------------------------------------------------

	BARONY
	File: filepc.hpp
	Desc: defines the PC-specific derived class for FileBase.

	Copyright 2013-2016 (c) Turning Wheel LLC, all rights reserved.
	See LICENSE for details.

-------------------------------------------------------------------------------*/

#include "../files.hpp"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <cstring>
#include <limits>
#include <vector>

//Don't create a FileBase or derivative class (such as this one) directly, use FileIO::open to get one...
class FilePC : public FileBase
{
	friend class FileIO;
public:
	size_t write(const void* src, size_t size, size_t count) override
	{
        if (0U == FileBase::write(src, size, count)
            || size == 0U
            || count == 0U
            || count > std::numeric_limits<size_t>::max() / size
            || pos > data.size())
        {
            return 0U;
        }
        const size_t writeSize = size * count;
        if (writeSize > data.max_size() - data.size())
        {
            return 0U;
        }
        (void)data.insert(data.begin() + pos, (const uint8_t*)src, (const uint8_t*)src + writeSize);
        pos += writeSize;
        return count;
    }

	size_t read(void* buffer, size_t size, size_t count) override
	{
        if (0U == FileBase::read(buffer, size, count)
            || size == 0U
            || count == 0U
            || count > std::numeric_limits<size_t>::max() / size
            || pos >= data.size())
        {
            return 0U;
        }
        const size_t requestedSize = size * count;
        const size_t readSize = std::min(requestedSize, data.size() - pos);
        std::memcpy(buffer, data.data() + pos, readSize);
        pos += readSize;
        return readSize / size;
    }

	size_t size() override
	{
		return data.size();
	}

	bool eof() override
	{
		return pos >= size();
	}

	int seek(ptrdiff_t offset, SeekMode mode) override
	{
        size_t base = 0U;
        switch (mode)
        {
            case SeekMode::SET:
                base = 0U;
                break;
            case SeekMode::ADD:
                base = pos;
                break;
            case SeekMode::SETEND:
                base = data.size();
                break;
            default:
                return -1;
        }

        size_t target = base;
        if (offset < 0)
        {
            const size_t distance = static_cast<size_t>(-(offset + 1)) + 1U;
            if (distance > base)
            {
                return -1;
            }
            target = base - distance;
        }
        else
        {
            const size_t distance = static_cast<size_t>(offset);
            if (distance > data.size() - std::min(base, data.size()))
            {
                return -1;
            }
            target = base + distance;
        }

        if (target > data.size())
        {
            return -1;
        }
        pos = target;
        return eof() ? -1 : 0;
    }

	long int tell() override
	{
		return (long int)pos;
	}

private:
	FilePC(FILE* fp, FileMode mode, const char* path) :
		FileBase(mode, path),
		fp(fp)
	{
	    assert(fp);
	    if (mode == FileMode::READ) {
		    (void)fseek(fp, 0, SEEK_END);
		    size_t end = ftell(fp);
		    (void)fseek(fp, 0, SEEK_SET);
		    data.resize(end);
		    size_t c = 0;
		    for (; c < end;) {
                size_t result = fread(data.data() + c, sizeof(uint8_t), end - c, fp);
		        if (!result) {
		            // failed to read, try to read just a chunk
		            constexpr size_t chunk_size = 1024;
		            size_t chunk = std::min(end - c, chunk_size);
		            printlog("[FILES] failed to read %llu bytes from '%s', trying %llu bytes instead", end - c, path, chunk);
                    result = fread(data.data() + c, sizeof(uint8_t), chunk, fp);
                    if (!result) {
                        printlog("[FILES] unable to continue reading '%s'", path);
                        data.resize(c);
                        break;
                    }
		        }
		        c += result;
		    }
	        assert(c == end);
		}
	}

	~FilePC()
	{
	}

	void close() override
	{
	    assert(fp);
	    if (mode == FileMode::WRITE) {
	        size_t c = 0u;
	        size_t end = size();
		    for (; c < end;) {
                size_t result = fwrite(data.data() + c, sizeof(uint8_t), end - c, fp);
		        if (!result) {
		            // failed to write, try to write just a chunk
		            constexpr size_t chunk_size = 1024;
		            size_t chunk = std::min(end - c, chunk_size);
		            printlog("[FILES] failed to write %llu bytes to '%s', trying %llu bytes instead", end - c, path.c_str(), chunk);
                    result = fwrite(data.data() + c, sizeof(uint8_t), chunk, fp);
                    if (!result) {
                        printlog("[FILES] unable to continue writing '%s'", path.c_str());
                        break;
                    }
		        }
		        c += result;
		    }
	        assert(c == end);
	    }
		int result = fclose(fp);
		assert(result == 0);
	}

	FILE* fp = nullptr;
	std::vector<uint8_t> data;
	size_t pos = 0u;
};
