//
// Created by maros on 15.3.2023.
//

#include <format>
#include <zlib.h>

#include <cstdio>
#include <stdexcept>

#include "libpak/algorithms.hpp"
#include "libpak/libpak.hpp"
#include "libpak/util.hpp"

#include <ranges>

bool libpak::stream::read(uint8_t* buffer, int64_t size, int64_t offset, std::ios::seekdir dir)
{
  if (this->source == nullptr || this->source->fail())
    throw std::runtime_error("stream source is not available");

  int64_t origin = 0;
  if (offset != 0)
  {
    origin = this->source->tellg();
    this->source->seekg(offset, dir);
  }

  this->source->read(reinterpret_cast<char*>(buffer), size);
  if (origin != 0)
    this->source->seekg(origin);

  return this->source->good();
}

bool libpak::stream::write(
    const uint8_t* buffer, int64_t size, int64_t offset, std::ios::seekdir dir)
{
  if (this->sink == nullptr)
    throw std::runtime_error("stream sink is not available");

  int64_t origin = 0;
  if (offset != 0)
  {
    origin = this->sink->tellp();
    this->sink->seekp(offset, dir);
  }

  this->sink->write(reinterpret_cast<const char*>(buffer), size);
  if (origin != 0)
    this->sink->seekp(origin);

  return this->sink->good();
}

int64_t libpak::stream::set_writer_cursor(int64_t pos, std::ios::seekdir dir)
{
  if (this->sink == nullptr)
    return -1;
  auto origin = get_writer_cursor();
  this->sink->seekp(pos, dir);
  return origin;
}

int64_t libpak::stream::get_writer_cursor()
{
  if (this->sink == nullptr)
    return -1;
  return this->sink->tellp();
}

int64_t libpak::stream::set_reader_cursor(int64_t pos, std::ios::seekdir dir)
{
  if (this->source == nullptr)
    return -1;
  auto origin = get_reader_cursor();
  this->source->seekg(pos, dir);
  return origin;
}

int64_t libpak::stream::get_reader_cursor()
{
  if (this->source == nullptr)
    return -1;
  return this->source->tellg();
}

libpak::stream::stream(
    const std::shared_ptr<std::istream>& source, const std::shared_ptr<std::ostream>& sink)
    : source(source), sink(sink)
{
}

void libpak::resource::create() {}

void libpak::resource::read(bool data)
{
  // input stream
  this->input_stream = std::make_shared<std::ifstream>(this->resource_path, std::ios::binary);
  // resource stream wrapper
  this->resource_stream = std::make_shared<stream>(this->input_stream, this->output_stream);

  // reset to known state
  {
    this->resource_stream->set_reader_cursor(0);
  }

  // read the intro header
  if (!this->resource_stream->read(this->pak_header))
    throw std::runtime_error("failed to read pak header");

  // read the content header
  this->resource_stream->set_reader_cursor(PAK_CONTENT_SECTOR);
  if (!this->resource_stream->read(this->content_header))
    throw std::runtime_error("failed to read content header");

  // reserve the size of asset count
  this->assets.reserve(this->content_header.assets_count);

  const uint32_t registeredAssetCount = content_header.assets_count;

  // read the assets
  for (uint32_t assetIndex{0}; assetIndex < registeredAssetCount; assetIndex++)
  {
    try
    {
      asset asset;

      // read asset
      this->read_asset(asset, data);
      // index asset
      this->assets[asset.path()] = asset;
    }
    catch (const std::runtime_error& e)
    {
      throw std::runtime_error(std::format("failed to read asset: {}", e.what()));
    }
  }
}

void libpak::resource::write()
{
  // input stream
  this->output_stream = std::make_shared<std::ofstream>(this->resource_path, std::ios::binary);
  // resource stream wrapper
  this->resource_stream = std::make_shared<stream>(this->input_stream, this->output_stream);

  this->resource_stream->set_writer_cursor(0);

  // Update the content header
  this->content_header.assets_count = this->assets.size();

  // write the content header
  this->resource_stream->set_writer_cursor(PAK_CONTENT_SECTOR);
  if (!this->resource_stream->write(this->content_header))
    throw std::runtime_error("failed to read content header");

  int64_t data_offset = 0;
  for (auto& [path, asset] : this->assets)
  {
    this->write_asset(asset);

    asset.header.embedded_data_offset = PAK_DATA_SECTOR + data_offset;

    auto origin = this->resource_stream->set_writer_cursor(asset.header.asset_offset);
    this->write_asset_data(asset);
    // Offset the data cursor by the length of the embedded data.
    data_offset += asset.header.embedded_data_length;

    // Return to the asset header origin
    this->resource_stream->set_writer_cursor(origin);
  }

  if (!this->resource_stream->write(this->data_header))
    throw std::runtime_error("failed to write data header");

  // Update the intro PAKS header assets counts
  this->pak_header.assets_count = this->assets.size();
  this->pak_header.used_assets_count = this->assets.size();
  this->pak_header.deleted_assets_count = 0;

  this->pak_header.file_size = this->resource_stream->get_writer_cursor();

  // Write the intro PAKS header
  this->resource_stream->set_writer_cursor(0);
  if (!this->resource_stream->write(this->pak_header))
    throw std::runtime_error("failed to write pak header");

  this->output_stream->close();
}

void libpak::resource::read_asset(libpak::asset& asset, bool data)
{

  auto& header = asset.header;
  // read asset header
  if (!this->resource_stream->read(header, header.asset_offset))
    throw std::runtime_error("failed to read asset header");

  // handle invalid asset
  if (header.asset_magic == 0x0)
    throw std::runtime_error("invalid asset header read");

  if (data)
  {
    try
    {
      this->read_asset_data(asset);
    }
    catch (const std::runtime_error& err)
    {
      throw std::runtime_error(std::format("failed read asset data: {}", err.what()));
    }
  }
}

void libpak::resource::read_asset_data(libpak::asset& asset)
{
  auto& header = asset.header;
  auto& data = asset.data;
  if (!header.is_asset_embedded)
    return;

  uLongf embeddedSize = asset.header.embedded_data_length;
  uint64_t embeddedDataOffset = header.embedded_data_offset;

  // allocate embedded data buffer
  std::shared_ptr<uint8_t> embeddedData(nullptr);
  try
  {
    embeddedData.reset(new uint8_t[embeddedSize]);
  }
  catch (std::bad_alloc& alloc)
  {
    throw std::runtime_error("not enough memory for embedded buffer");
  }

  // read embedded data
  if (!this->resource_stream->read(embeddedData.get(), embeddedSize, embeddedDataOffset))
    throw std::runtime_error("couldn't read embedded data");

  // if data is not compressed, return the unprocessed buffer
  if (!header.is_data_compressed)
  {
    asset.data.buffer = embeddedData;
    return;
  }

  // npak can compress small buffers and inflate them
  // due to this, the largest data length has to be chosen for the uncompressed data buffer
  uLongf dataSize = std::max(header.embedded_data_length, header.data_decompressed_length);
  // allocate buffer for data
  try
  {
    data.buffer.reset(new uint8_t[dataSize]);
  }
  catch (std::bad_alloc& alloc)
  {
    throw std::runtime_error("not enough memory for data buffer");
  }

  // uncompress
  auto result = uncompress2(data.buffer.get(), &dataSize, embeddedData.get(), &embeddedSize);
  switch (result)
  {
  case Z_BUF_ERROR:
  case Z_MEM_ERROR:
    throw std::runtime_error("not enough memory for uncompressed data");
  case Z_DATA_ERROR:
    throw std::runtime_error("corrupted compressed data");
  default:
  {
  };
  break;
  }
}

void libpak::resource::write_asset(const libpak::asset& asset) {}

void libpak::resource::write_asset_data(const libpak::asset& asset) {}

void libpak::resource::destroy() noexcept
{
  this->pak_header = {};
  this->content_header = {};
  this->data_header = {};
  this->assets.clear();
}
