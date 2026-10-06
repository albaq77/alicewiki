
// 是否让 SD 卡按块读取走 SDMMC 内部 DMA（阻塞式封装 ReadDMA_Blocking）。
#ifndef IMAGE_CARD_USE_DMA
#define IMAGE_CARD_USE_DMA 0
#endif

#if IMAGE_PROFILE
struct ImageProfile {
	stduint card_read_call_count;
	stduint card_read_block_count;
	stduint card_read_bytes;
	stduint card_read_fallback_count;
	stduint sd_read_count;
	stduint sd_read_bytes;
	stduint sd_byte_count;
	stduint cache_hit_count;
	stduint cache_miss_count;
	stduint cache_evict_count;
	stduint png_rows;
	stduint png_pixels;
	uint64 card_read_ms;
	uint64 sd_read_ms;
	uint64 search_ms;
	uint64 decode_ms;
	uint64 scale_ms;
	uint64 png_open_ms;
	uint64 png_read_ms;
	uint64 png_draw_ms;
};

static ImageProfile image_profile;

static void profile_reset() {
	MemSet(&image_profile, 0, sizeof(image_profile));
}

static uint64 profile_now() {
	return SysTick::getTick();
}
#endif

#if IMAGE_CARD_CACHE
struct ImageCardCacheEntry {
	bool valid;
	stduint block;
	uint32 age;
};

// 32B 对齐：内部 DMA 与 D-cache 维护都按 cache line 操作（Cortex-M7 行宽 32B）。
static ImageCardCacheEntry image_card_cache[IMAGE_CARD_CACHE_BLOCKS];
alignas(32) static byte image_card_cache_data[IMAGE_CARD_CACHE_BYTES];
alignas(32) static byte image_card_readahead_data[IMAGE_CARD_READAHEAD_BYTES];

class CachedStorageDevice : public StorageTrait {
private:
	SecureDigitalCard_t* storage;
	uint32 age_clock;

	byte* cache_data(stduint index) {
		return image_card_cache_data + index * IMAGE_CARD_BLOCK_SIZE;
	}

	stduint choose_slot() {
		stduint oldest = 0;
		for (stduint i = 0; i < IMAGE_CARD_CACHE_BLOCKS; ++i) {
			if (!image_card_cache[i].valid) return i;
			if (image_card_cache[i].age < image_card_cache[oldest].age) oldest = i;
		}
#if IMAGE_PROFILE
		image_profile.cache_evict_count++;
#endif
		return oldest;
	}

	void fill_slot(stduint slot, stduint block, const byte* data) {
		MemCopyN(cache_data(slot), data, IMAGE_CARD_BLOCK_SIZE);
		image_card_cache[slot].valid = true;
		image_card_cache[slot].block = block;
		image_card_cache[slot].age = ++age_clock;
	}

	bool read_blocks(stduint block, stduint count, byte* data) {
#if IMAGE_PROFILE
		uint64 t0 = profile_now();
#endif
#if IMAGE_CARD_USE_DMA
		// 阻塞式内部 DMA：返回时数据已搬完，且已做过 D-cache 维护
		bool ok = storage->ReadDMA_Blocking(data, block, count, IMAGE_CARD_READ_TIMEOUT_MS, nullptr);
#else
		bool ok = storage->Read(data, block, count, IOMethod::Loop, IMAGE_CARD_READ_TIMEOUT_MS, nullptr);
#endif
#if IMAGE_PROFILE
		image_profile.card_read_call_count++;
		if (ok) {
			image_profile.card_read_block_count += count;
			image_profile.card_read_bytes += count * IMAGE_CARD_BLOCK_SIZE;
		}
		image_profile.card_read_ms += profile_now() - t0;
#endif
		return ok;
	}

public:
	using BlockTrait::Read;
	using BlockTrait::Write;

	CachedStorageDevice(SecureDigitalCard_t& inner) : storage(&inner), age_clock(0) {
		Block_Size = inner.Block_Size;
		readable = inner.readable;
		writable = inner.writable;
		for (stduint i = 0; i < IMAGE_CARD_CACHE_BLOCKS; ++i) {
			image_card_cache[i].valid = false;
			image_card_cache[i].block = IMAGE_CARD_CACHE_INVALID_BLOCK;
			image_card_cache[i].age = 0;
		}
	}

	bool Read(stduint BlockIden, void* Dest, stduint Times = 1) override {
		for0(t, Times) {
			stduint blk = BlockIden + t;
			byte* dst = (byte*)Dest + t * Block_Size;
			bool found = false;
			for (stduint i = 0; i < IMAGE_CARD_CACHE_BLOCKS; ++i) {
				if (image_card_cache[i].valid && image_card_cache[i].block == blk) {
					image_card_cache[i].age = ++age_clock;
					MemCopyN(dst, cache_data(i), IMAGE_CARD_BLOCK_SIZE);
#if IMAGE_PROFILE
					image_profile.cache_hit_count++;
#endif
					found = true;
					break;
				}
			}
			if (found) continue;

#if IMAGE_PROFILE
			image_profile.cache_miss_count++;
#endif

#if IMAGE_CARD_READAHEAD
			stduint count = IMAGE_CARD_READAHEAD_BLOCKS;
			stduint units = getUnits();
			if (blk + count > units) count = units - blk;
			bool readahead_ok = count && read_blocks(blk, count, image_card_readahead_data);
			if (readahead_ok) {
				for (stduint i = 0; i < count; ++i) {
					stduint slot = choose_slot();
					fill_slot(slot, blk + i, image_card_readahead_data + i * IMAGE_CARD_BLOCK_SIZE);
				}
				MemCopyN(dst, image_card_readahead_data, IMAGE_CARD_BLOCK_SIZE);
				continue;
			}
#if IMAGE_PROFILE
			image_profile.card_read_fallback_count++;
#endif
#endif

			stduint slot = choose_slot();
			byte* data = cache_data(slot);
			bool ok = read_blocks(blk, 1, data);
			if (!ok) {
				image_card_cache[slot].valid = false;
				return false;
			}
			fill_slot(slot, blk, data);
			MemCopyN(dst, data, IMAGE_CARD_BLOCK_SIZE);
		}
		return true;
	}

	bool Write(stduint BlockIden, const void* Sors, stduint Times = 1) override {
		for0(j, Times) {
			stduint blk = BlockIden + j;
			for (stduint i = 0; i < IMAGE_CARD_CACHE_BLOCKS; ++i) {
				if (image_card_cache[i].valid && image_card_cache[i].block == blk) {
					image_card_cache[i].valid = false;
					break;
				}
			}
		}
		return storage->Write(BlockIden, Sors, Times);
	}

	stduint getUnits() override {
		return storage->getUnits();
	}

	int operator[](uint64 bytid) override {
		byte block[IMAGE_CARD_BLOCK_SIZE];
		if (!Read((stduint)(bytid / IMAGE_CARD_BLOCK_SIZE), block)) return -1;
		return block[bytid % IMAGE_CARD_BLOCK_SIZE];
	}
};
#elif IMAGE_PROFILE
class ProfileStorageDevice : public StorageTrait {
private:
	StorageTrait* storage;
public:
	using BlockTrait::Read;
	using BlockTrait::Write;

	ProfileStorageDevice(StorageTrait& inner) : storage(&inner) {
		Block_Size = inner.Block_Size;
		readable = inner.readable;
		writable = inner.writable;
	}

	bool Read(stduint BlockIden, void* Dest, stduint Times = 1) override {
		uint64 t0 = profile_now();
		bool ok = storage->Read(BlockIden, Dest, Times);
		image_profile.card_read_call_count++;
		if (ok) {
			image_profile.card_read_block_count += Times;
			image_profile.card_read_bytes += Block_Size * Times;
		}
		image_profile.card_read_ms += profile_now() - t0;
		return ok;
	}

	bool Write(stduint BlockIden, const void* Sors, stduint Times = 1) override {
		return storage->Write(BlockIden, Sors, Times);
	}

	stduint getUnits() override {
		return storage->getUnits();
	}

	int operator[](uint64 bytid) override {
		return (*storage)[bytid];
	}
};
#endif




#if IMAGE_PROFILE
inline static void profile_print(const char* name, ImageResult r) {
	XART1.OutFormat(
		"prof %s r=%d search=%ums decode=%ums scale=%ums file=%u/%uB/%ums byte=%u card=%u/%u/%uB/%ums fb=%u cache=%u/%u/%u\r\n",
		name,
		(int)r,
		(unsigned)image_profile.search_ms,
		(unsigned)image_profile.decode_ms,
		(unsigned)image_profile.scale_ms,
		(unsigned)image_profile.sd_read_count,
		(unsigned)image_profile.sd_read_bytes,
		(unsigned)image_profile.sd_read_ms,
		(unsigned)image_profile.sd_byte_count,
		(unsigned)image_profile.card_read_call_count,
		(unsigned)image_profile.card_read_block_count,
		(unsigned)image_profile.card_read_bytes,
		(unsigned)image_profile.card_read_ms,
		(unsigned)image_profile.card_read_fallback_count,
		(unsigned)image_profile.cache_hit_count,
		(unsigned)image_profile.cache_miss_count,
		(unsigned)image_profile.cache_evict_count
	);
	if (image_profile.png_rows) {
		XART1.OutFormat(
			"prof png open=%ums read=%ums draw=%ums rows=%u pixels=%u\r\n",
			(unsigned)image_profile.png_open_ms,
			(unsigned)image_profile.png_read_ms,
			(unsigned)image_profile.png_draw_ms,
			(unsigned)image_profile.png_rows,
			(unsigned)image_profile.png_pixels
		);
	}
}
#endif


