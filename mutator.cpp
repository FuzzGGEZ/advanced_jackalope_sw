/*
Copyright 2020 Google LLC

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    https://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
*/

#include "stdlib.h"
#include "string.h"
#include "common.h"
#include "mutator.h"

#include "ctype.h"
#include <algorithm>
#include <iostream>
#include <fstream>
#include <stdint.h>
#include <vector>

Mutex RepeatMutator::stats_mutex;
uint64_t RepeatMutator::stats[REPEAT_STATS];
uint64_t RepeatMutator::nstats = 0;
uint64_t RepeatMutator::next_stat = 0;
uint64_t RepeatMutator::median_num_repeats = 2;
float RepeatMutator::adapted_repeat_p = 0.75;

int Mutator::GetRandBlock(size_t samplesize, size_t minblocksize, size_t maxblocksize, size_t *blockstart, size_t *blocksize, PRNG *prng) {
  if (samplesize == 0) return 0;
  if (samplesize < minblocksize) return 0;
  if (samplesize < maxblocksize) maxblocksize = samplesize;
  *blocksize = prng->Rand((int)minblocksize, (int)maxblocksize);
  *blockstart = prng->Rand(0, (int)(samplesize - (*blocksize)));
  return 1;
}

bool ByteFlipMutator::Mutate(Sample *inout_sample, PRNG *prng, std::vector<Sample *> &all_samples) {
  // printf("In ByteFlipMutator::Mutate\n");
  if (inout_sample->size == 0) return true;
  int charpos = prng->Rand(0, (int)(inout_sample->size - 1));
  char c = (char)prng->Rand(0, 255);
  inout_sample->bytes[charpos] = c;
  return true;
}

bool ArithmeticMutator::Mutate(Sample *inout_sample,
                               PRNG *prng,
                               std::vector<Sample *> &all_samples)
{
  int flip_endian = prng->Rand(0, 1);
  int size = prng->Rand(0, 2);
  switch(size) {
    case 0:
      return MutateArithmeticValue<uint16_t>(inout_sample, prng, flip_endian);
    case 1:
      return MutateArithmeticValue<uint32_t>(inout_sample, prng, flip_endian);
    case 2:
      return MutateArithmeticValue<uint64_t>(inout_sample, prng, flip_endian);
  }
  return true;
}

template<typename T>
bool ArithmeticMutator::MutateArithmeticValue(Sample *inout_sample,
                                              PRNG *prng,
                                              int flip_endian)
{
  T value;
  size_t blockstart, blocksize;
  if (!GetRandBlock(inout_sample->size,
                    sizeof(T), sizeof(T),
                    &blockstart, &blocksize,
                    prng))
    return true;
  value = *(T *)(inout_sample->bytes + blockstart);
  if(flip_endian) value = FlipEndian(value);
  int change = prng->Rand(-256, 256);
  value += change;
  if(flip_endian) value = FlipEndian(value);
  *(T *)(inout_sample->bytes + blockstart) = value;
  return true;
}

bool BlockFlipMutator::Mutate(Sample *inout_sample, PRNG *prng, std::vector<Sample *> &all_samples) {
  // printf("In BlockFlipMutator::Mutate\n");
  size_t blocksize, blockpos;
  if (!GetRandBlock(inout_sample->size, min_block_size, max_block_size, &blockpos, &blocksize, prng)) return true;
  if (uniform) {
    char c = (char)prng->Rand(0, 255);
    for (size_t i = 0; i<blocksize; i++) {
      inout_sample->bytes[blockpos + i] = c;
    }
  } else {
    for (size_t i = 0; i<blocksize; i++) {
      inout_sample->bytes[blockpos + i] = (char)prng->Rand(0, 255);
    }
  }
  return true;
}

bool AppendMutator::Mutate(Sample *inout_sample, PRNG *prng, std::vector<Sample *> &all_samples) {
  // printf("In AppendMutator::Mutate\n");
  size_t old_size = inout_sample->size;
  if (old_size >= Sample::max_size) return true;
  size_t append = prng->Rand(min_append, max_append);
  if ((old_size + append) > Sample::max_size) {
    append = Sample::max_size - old_size;
  }
  if (append <= 0) return true;
  size_t new_size = old_size + append;
  char *new_bytes = (char *)realloc(inout_sample->bytes, new_size);
  if (!new_bytes) {
    FATAL("realloc failed in appendmutator");
  }
  inout_sample->bytes = new_bytes;
  inout_sample->size = new_size;
  for (size_t i = old_size; i < new_size; i++) {
    inout_sample->bytes[i] = (char)prng->Rand(0, 255);
  }
  return true;
}

bool BlockInsertMutator::Mutate(Sample *inout_sample, PRNG *prng, std::vector<Sample *> &all_samples) {
  // printf("In BlockInsertMutator::Mutate\n");
  size_t old_size = inout_sample->size;
  if (old_size >= Sample::max_size) return true;
  size_t to_insert = prng->Rand(min_insert, max_insert);
  if ((old_size + to_insert) > Sample::max_size) {
    to_insert = Sample::max_size - old_size;
  }
  size_t where = prng->Rand(0, (int)old_size);
  size_t new_size = old_size + to_insert;
  if (to_insert <= 0) return true;
  
  char *old_bytes = inout_sample->bytes;
  char *new_bytes = (char *)malloc(new_size);
  memcpy(new_bytes, old_bytes, where);
  
  for (size_t i = 0; i < to_insert; i++) {
    new_bytes[where + i] = (char)prng->Rand(0, 255);
  }
  
  memcpy(new_bytes + where + to_insert, old_bytes + where, old_size - where);

  if (old_bytes) free(old_bytes);
  inout_sample->bytes = new_bytes;
  inout_sample->size = new_size;
  return true;
}

bool BlockDuplicateMutator::Mutate(Sample *inout_sample, PRNG *prng, std::vector<Sample *> &all_samples) {
  // printf("In BlockDuplicateMutator::Mutate\n");
  if (inout_sample->size >= Sample::max_size) return true;
  size_t blockpos, blocksize;
  if (!GetRandBlock(inout_sample->size, min_block_size, max_block_size, &blockpos, &blocksize, prng)) return true;
  int64_t blockcount = prng->Rand(min_duplicate_cnt, max_duplicate_cnt);
  if ((inout_sample->size + blockcount * blocksize) > Sample::max_size)
    blockcount = (Sample::max_size - (int64_t)inout_sample->size) / blocksize;
  if (blockcount <= 0) return true;
  char *newbytes;
  newbytes = (char *)malloc(inout_sample->size + blockcount * blocksize);
  memcpy(newbytes, inout_sample->bytes, blockpos + blocksize);
  for (int64_t i = 0; i<blockcount; i++) {
    memcpy(newbytes + blockpos + (i + 1)*blocksize, inout_sample->bytes + blockpos, blocksize);
  }
  memcpy(newbytes + blockpos + (blockcount + 1)*blocksize, 
         inout_sample->bytes + blockpos + blocksize,
         inout_sample->size - blockpos - blocksize);
  if (inout_sample->bytes) free(inout_sample->bytes);
  inout_sample->bytes = newbytes;
  inout_sample->size = inout_sample->size + blockcount * blocksize;
  return true;
}

void Mutator::AddInterestingValue(char *data, size_t size, std::vector<Sample>& interesting_values) {
  Sample interesting_sample;
  interesting_sample.Init(data, size);
  interesting_values.push_back(interesting_sample);
}

bool InterestingValueMutator::Mutate(Sample *inout_sample, PRNG *prng, std::vector<Sample *> &all_samples) {
  // printf("In InterestingValueMutator::Mutate\n");
  if (interesting_values.empty()) return true;
  Sample *interesting_sample = &interesting_values[prng->Rand(0, (int)interesting_values.size() - 1)];
  size_t blockstart, blocksize;
  if (!GetRandBlock(inout_sample->size, interesting_sample->size, interesting_sample->size, &blockstart, &blocksize, prng)) return true;
  memcpy(inout_sample->bytes + blockstart, interesting_sample->bytes, interesting_sample->size);
  return true;
}

InterestingValueMutator::InterestingValueMutator(bool use_default_values) {
  if (use_default_values) {
    AddDefaultInterestingValues<uint16_t>(interesting_values);
    AddDefaultInterestingValues<uint32_t>(interesting_values);
    // AddDefaultInterestingValues<uint64_t>(interesting_values);
  }
}

template<typename T> void Mutator::AddDefaultInterestingValues(std::vector<Sample>& interesting_values) {
  uint32_t M[] = {2, 3, 4, 6, 8, 10, 12, 16, 24, 32, 40, 48,
                  56, 64, 72, 80, 88, 96, 104, 112, 120, 128,
                  136, 144, 152, 160, 168, 176, 184, 192, 200,
                  208, 216, 224, 232, 240, 248, 256 };

  int32_t N[] = {1, 2, 3, 4, 6, 8, 10, 12, 16, 32, 64, 128, 256};

  T value;
  value = 0;
  AddInterestingValue((char *)(&value), sizeof(value), interesting_values);

  value = 1;
  for (uint32_t i = 0; i < (sizeof(value) * 8); i++) {
    AddInterestingValue((char *)(&value), sizeof(value), interesting_values);
    value = (value << 1);
  }

  for (uint32_t i = 0; i < (sizeof(M)/sizeof(M[0])); i++) {
    int32_t m = M[i];
    value = (T)(-1) / m + 1;
    AddInterestingValue((char *)(&value), sizeof(value), interesting_values);
    value = FlipEndian(value);
    AddInterestingValue((char *)(&value), sizeof(value), interesting_values);
  }
    
  for (uint32_t j = 0; j < (sizeof(N)/sizeof(N[0])); j++) {
    int32_t n = N[j];
    value = (T)(0) - n;
    AddInterestingValue((char *)(&value), sizeof(value), interesting_values);
    value = FlipEndian(value);
    AddInterestingValue((char *)(&value), sizeof(value), interesting_values);
  }
}

void InterestingValueMutator::DictUnescape(std::string &in, std::string &out) {
  const char* in_buf = in.data();
  char* out_buf = (char*)malloc(in.size());
  size_t in_pos = 0, out_pos = 0;
  size_t in_size = in.size();

  char convert_buf[3];
  convert_buf[2] = 0;

  if (in_size < 4) {
    out = in;
    return;
  }

  while (in_pos < (in_size - 3)) {
    if((in_buf[in_pos] == '\\') && (in_buf[in_pos + 1] == 'x') &&
       isxdigit(in_buf[in_pos + 2]) && isxdigit(in_buf[in_pos + 3]))
    {
      convert_buf[0] = in_buf[in_pos + 2];
      convert_buf[1] = in_buf[in_pos + 3];
      out_buf[out_pos] = (char)strtol(convert_buf, NULL, 16);
      in_pos += 4;
      out_pos++;
    } else {
      out_buf[out_pos] = in_buf[in_pos];
      out_pos++;
      in_pos++;
    }
  }

  while (in_pos < in_size) {
    out_buf[out_pos] = in_buf[in_pos];
    out_pos++;
    in_pos++;
  }

  out.assign(out_buf, out_pos);
  free(out_buf);
}

void InterestingValueMutator::AddDictionary(char* path) {
  std::fstream f;
  f.open(path, std::ios::in);
  if (!f.is_open()) {
    FATAL("Error reading %s", path);
  }

  size_t values_added = 0;

  std::string line;
  std::string escapepattern = "\\x";
  while (getline(f, line)) {
    if (line.empty()) continue;
    if (line.find(escapepattern) == std::string::npos) {
      AddValue(line.data(), line.size());
    } else {
      std::string unescaped;
      DictUnescape(line, unescaped);
      AddValue(unescaped.data(), unescaped.size());
    }
    values_added++;
  }

  f.close();

  printf("Added %zu values from dictionary\n", values_added);
}

bool SpliceMutator::Mutate(Sample *inout_sample, PRNG *prng, std::vector<Sample *> &all_samples) {
  if(all_samples.empty()) return true;

  bool displace = false;
  if(prng->RandReal() < displacement_p) {
    displace = true;
  }
  
  Sample *other_sample = all_samples[prng->Rand(0, (int)all_samples.size() - 1)];

  if(inout_sample->size == 0) return false;
  if(other_sample->size == 0) return false;

  if(points == 1) {
    size_t point1, point2;
    char *new_bytes;
    size_t new_sample_size;
    if(displace) {
      point1 = prng->Rand(0, (int)(inout_sample->size - 1));
      point2 = prng->Rand(0, (int)(other_sample->size - 1));
    } else {
      size_t minsize = inout_sample->size;
      if(other_sample->size < minsize) minsize = other_sample->size;
      point1 = prng->Rand(0, (int)(minsize - 1));
      point2 = point1;
    }
    new_sample_size = point1 + (other_sample->size - point2);
    if(new_sample_size == inout_sample->size) {
      memcpy(inout_sample->bytes + point1, other_sample->bytes + point2, other_sample->size - point2);
      return true;
    } else {
      new_bytes = (char *)malloc(new_sample_size);
      memcpy(new_bytes, inout_sample->bytes, point1);
      memcpy(new_bytes + point1, other_sample->bytes + point2, other_sample->size - point2);
      free(inout_sample->bytes);
      inout_sample->bytes = new_bytes;
      inout_sample->size = new_sample_size;
      if (inout_sample->size > Sample::max_size) inout_sample->Trim(Sample::max_size);
      return true;
    }
  } else if(points != 2) {
    FATAL("Splice mutator can only work with 1 or 2 splice points");
  }
  
  if(displace) {
    size_t blockstart1, blocksize1;
    size_t blockstart2, blocksize2;
    size_t blockstart3, blocksize3;
    if(!GetRandBlock(inout_sample->size, 1, inout_sample->size, &blockstart1, &blocksize1, prng)) return true;
    if(!GetRandBlock(other_sample->size, 1, other_sample->size, &blockstart2, &blocksize2, prng)) return true;
    blockstart3 = blockstart1 + blocksize1;
    blocksize3 = inout_sample->size - blockstart3;
    size_t new_sample_size = blockstart1 + blocksize2 + blocksize3;
    char *new_bytes = (char *)malloc(new_sample_size);
    memcpy(new_bytes, inout_sample->bytes, blockstart1);
    memcpy(new_bytes + blockstart1, other_sample->bytes + blockstart2, blocksize2);
    memcpy(new_bytes + blockstart1 + blocksize2, inout_sample->bytes + blockstart3, blocksize3);
    if(new_sample_size > Sample::max_size) {
      new_sample_size = Sample::max_size;
      new_bytes = (char *)realloc(new_bytes, Sample::max_size);
    }
    free(inout_sample->bytes);
    inout_sample->bytes = new_bytes;
    inout_sample->size = new_sample_size;
    return true;
  } else {
    size_t blockstart, blocksize;
    if(!GetRandBlock(other_sample->size, 2, other_sample->size, &blockstart, &blocksize, prng)) return true;
    if(blockstart > inout_sample->size) {
      blocksize += (blockstart - inout_sample->size);
      blockstart = inout_sample->size;
    }
    if((blockstart + blocksize) <= inout_sample->size) {
      memcpy(inout_sample->bytes + blockstart, other_sample->bytes + blockstart, blocksize);
      return true;
    }
    size_t new_sample_size = blockstart + blocksize;
    char *new_bytes = (char *)malloc(new_sample_size);
    memcpy(new_bytes, inout_sample->bytes, blockstart);
    memcpy(new_bytes + blockstart, other_sample->bytes + blockstart, blocksize);
    free(inout_sample->bytes);
    inout_sample->bytes = new_bytes;
    inout_sample->size = new_sample_size;
    return true;
  }
}

void BaseDeterministicContext::AddHotOffset(size_t offset) {
  mutex.Lock();

  // in any case, restart scan
  cur_region = 0;
  
  MutateRegion new_region;
  new_region.cur_progress = 0;

  size_t newregion_start = offset;
  if(newregion_start < DETERMINISTIC_MUTATE_BYTES_PREVIOUS) newregion_start = 0;
  else newregion_start -= DETERMINISTIC_MUTATE_BYTES_PREVIOUS;
  size_t newregion_end = offset + DETERMINISTIC_MUTATE_BYTES_NEXT;
  
  for(auto iter = regions.begin(); iter != regions.end(); iter++) {
    if(newregion_start < iter->start) {
      new_region.start = newregion_start;
      new_region.cur = new_region.start;
      if(iter->start > newregion_end) {
        new_region.end = newregion_end;
      } else {
        new_region.end = iter->start;
      }
      regions.insert(iter, new_region);
      mutex.Unlock();
      return;
    }
    if(newregion_start <= iter->end) {
      if(newregion_end <= iter->end) {
        mutex.Unlock();
        return;
      }
      // extend an existing region
      iter->end = newregion_end;
      mutex.Unlock();
      return;
    }
  }
  new_region.start = newregion_start;
  new_region.cur = new_region.start;
  new_region.end = newregion_end;
  regions.push_back(new_region);
  mutex.Unlock();
  return;
}

bool BaseDeterministicContext::GetNextByteToMutate(size_t *pos, size_t *progress, size_t max_progress) {
  MutateRegion *region = NULL;
  
  while(cur_region < regions.size()) {
    region = &(regions[cur_region]);

    if(region->cur_progress >= max_progress) {
      region->cur_progress = 0;
      region->cur++;
    }

    if(region->cur >= region->end) {
      cur_region++;
      continue;
    }

    *pos = region->cur;
    *progress = region->cur_progress;
    region->cur_progress++;
    return true;
  }
  return false;
}


MutatorSampleContext *BaseDeterministicMutator::CreateSampleContext(Sample *sample) {
  BaseDeterministicContext *context = new BaseDeterministicContext;
  return context;
}

bool DeterministicByteFlipMutator::Mutate(Sample *inout_sample, PRNG *prng, std::vector<Sample *> &all_samples) {
  size_t pos;
  size_t value;
  
  if(!context->GetNextByteToMutate(&pos, &value, 256)) {
    return false;
  }
  
  if(pos >= inout_sample->size) {
    inout_sample->Resize(pos + 1);
  }
  inout_sample->bytes[pos] = (char)(value);
  
  return true;
}

DeterministicInterestingValueMutator::DeterministicInterestingValueMutator(bool use_default_values) {
  if (use_default_values) {
    AddDefaultInterestingValues<uint16_t>(interesting_values);
    AddDefaultInterestingValues<uint32_t>(interesting_values);
    // AddDefaultInterestingValues<uint64_t>(interesting_values);
  }
}

bool DeterministicInterestingValueMutator::Mutate(Sample *inout_sample, PRNG *prng, std::vector<Sample *> &all_samples) {
  size_t pos;
  size_t value_index;
  
  if(!context->GetNextByteToMutate(&pos, &value_index, interesting_values.size())) {
    return false;
  }
  
  Sample *interesting_sample = &interesting_values[value_index];
  if((pos + interesting_sample->size) > inout_sample->size) {
    inout_sample->Resize(pos + interesting_sample->size);
  }
  memcpy(inout_sample->bytes + pos, interesting_sample->bytes, interesting_sample->size);
  
  return true;
}

bool RangeMutator::Mutate(Sample* inout_sample, PRNG* prng, std::vector<Sample*>& all_samples) {
  Mutator* child_mutator = child_mutators[0];

  if (ranges->empty()) {
    return child_mutator->Mutate(inout_sample, prng, all_samples);
  }

  // pick a range
  Range& range = (*ranges)[prng->Rand() % ranges->size()];

  // printf("Mutating range %zd %zd\n", range.from, range.to);

  // extract the part we want to mutate
  Sample rangesample;
  inout_sample->Crop(range.from, range.to, &rangesample);

  // mutate the cropped sample (if not empty)
  if (inout_sample->size == 0) {
    return child_mutator->Mutate(inout_sample, prng, all_samples);
  } else {
    child_mutator->Mutate(&rangesample, prng, all_samples);
  }

  // put the cropped part back where it belongs
  if (range.from + rangesample.size > inout_sample->size) {
    inout_sample->Resize(range.from + rangesample.size);
  }
  memcpy(inout_sample->bytes + range.from, rangesample.bytes, rangesample.size);

  return true;
}

void RepeatMutator::UpdateStats() {
  stats_mutex.Lock();
  
  stats[next_stat] = last_num_repeats;
  next_stat = (next_stat + 1) % REPEAT_STATS;
  
  if(nstats >= REPEAT_STATS) {
    std::vector<size_t> sort_array;
    sort_array.assign(&(stats[0]), &(stats[REPEAT_STATS]));
    std::sort(sort_array.begin(), sort_array.end());
    median_num_repeats = sort_array[REPEAT_STATS/2];
    
    float new_adapted_repeat_p = 1.0f - 1.0f/median_num_repeats;
    if(new_adapted_repeat_p < 0.5) new_adapted_repeat_p = 0.5;
    
    if(new_adapted_repeat_p != adapted_repeat_p) {
      adapted_repeat_p = new_adapted_repeat_p;
      printf("Adjusting mutation repeat probability to %g\n", adapted_repeat_p);
    }
    
  } else {
    nstats++;
  }
  
  stats_mutex.Unlock();
}

void RepeatMutator::SaveGlobalState(FILE *fp) {
  stats_mutex.Lock();
  
  fwrite(stats, sizeof(stats), 1, fp);
  fwrite(&nstats, sizeof(nstats), 1, fp);
  fwrite(&next_stat, sizeof(next_stat), 1, fp);
  fwrite(&median_num_repeats, sizeof(median_num_repeats), 1, fp);
  fwrite(&adapted_repeat_p, sizeof(adapted_repeat_p), 1, fp);

  stats_mutex.Unlock();

  HierarchicalMutator::SaveGlobalState(fp);
}

void RepeatMutator::LoadGlobalState(FILE *fp) {
  stats_mutex.Lock();

  fread(stats, sizeof(stats), 1, fp);
  fread(&nstats, sizeof(nstats), 1, fp);
  fread(&next_stat, sizeof(next_stat), 1, fp);
  fread(&median_num_repeats, sizeof(median_num_repeats), 1, fp);
  fread(&adapted_repeat_p, sizeof(adapted_repeat_p), 1, fp);

  stats_mutex.Unlock();

  HierarchicalMutator::LoadGlobalState(fp);
}

bool BmpAwareMutator::IsLikelyBmp(Sample *sample) {
  if (sample->size < 54) return false;
  return sample->bytes[0] == 'B' && sample->bytes[1] == 'M';
}

uint16_t BmpAwareMutator::ReadLE16(Sample *sample, size_t off) {
  if (off + 2 > sample->size) return 0;
  return ((uint8_t)sample->bytes[off]) |
         ((uint8_t)sample->bytes[off + 1] << 8);
}

uint32_t BmpAwareMutator::ReadLE32(Sample *sample, size_t off) {
  if (off + 4 > sample->size) return 0;
  return ((uint8_t)sample->bytes[off]) |
         ((uint8_t)sample->bytes[off + 1] << 8) |
         ((uint8_t)sample->bytes[off + 2] << 16) |
         ((uint8_t)sample->bytes[off + 3] << 24);
}

void BmpAwareMutator::WriteLE16(Sample *sample, size_t off, uint16_t value) {
  if (off + 2 > sample->size) return;
  sample->bytes[off] = value & 0xff;
  sample->bytes[off + 1] = (value >> 8) & 0xff;
}

void BmpAwareMutator::WriteLE32(Sample *sample, size_t off, uint32_t value) {
  if (off + 4 > sample->size) return;
  sample->bytes[off] = value & 0xff;
  sample->bytes[off + 1] = (value >> 8) & 0xff;
  sample->bytes[off + 2] = (value >> 16) & 0xff;
  sample->bytes[off + 3] = (value >> 24) & 0xff;
}

bool BmpAwareMutator::Mutate(Sample *inout_sample, PRNG *prng, std::vector<Sample *> &all_samples) {
  if (!IsLikelyBmp(inout_sample)) return true;

  static const uint32_t interesting32[] = {
    0, 1, 2, 3, 4, 7, 8, 15, 16, 31, 32,
    63, 64, 127, 128, 255, 256, 257,
    511, 512, 1023, 1024, 2047, 2048,
    4095, 4096, 4097,
    0x7fffffff, 0x80000000, 0xffffffff,0xfffffffe,
    0xffff0000,0x80000001
  };

  static const uint16_t bitcounts[] = {
    1, 4, 8, 16, 24, 32
  };

  const size_t ninteresting = sizeof(interesting32) / sizeof(interesting32[0]);
  const size_t nbitcounts = sizeof(bitcounts) / sizeof(bitcounts[0]);

  uint32_t offbits = ReadLE32(inout_sample, 0x0A);
  int choice = prng->Rand(0, 18);

  switch (choice) {
    case 0:
      WriteLE32(inout_sample, 0x12, interesting32[prng->Rand() % ninteresting]);
      break;

    case 1:
      if (prng->Rand(0, 1)) {
        WriteLE32(inout_sample, 0x16, interesting32[prng->Rand() % ninteresting]);
      } else {
        WriteLE32(inout_sample, 0x16, 0xffffffff - prng->Rand(0, 4096));
      }
      break;

    case 2:
      WriteLE16(inout_sample, 0x1C, bitcounts[prng->Rand() % nbitcounts]);
      break;

    case 3:
      WriteLE32(inout_sample, 0x1E, prng->Rand(0, 3));
      break;

    case 4:
      WriteLE32(inout_sample, 0x22, interesting32[prng->Rand() % ninteresting]);
      break;

    case 5:
      WriteLE32(inout_sample, 0x0A, 54 + prng->Rand(0, 2048));
      break;

    case 6:
      WriteLE32(inout_sample, 0x02, (uint32_t)inout_sample->size + prng->Rand(0, 4096));
      break;

    case 7:
      WriteLE32(inout_sample, 0x2E, prng->Rand(0, 512));
      break;

    case 8:
      WriteLE16(inout_sample, 0x1A, prng->Rand(0, 4));
      break;

    case 9:
      WriteLE32(inout_sample, 0x0E, interesting32[prng->Rand() % ninteresting]);
      break;

    case 10:
      if (offbits < inout_sample->size) {
        size_t pos = offbits + (prng->Rand() % (inout_sample->size - offbits));
        inout_sample->bytes[pos] ^= (1 << (prng->Rand() % 8));
      }
      break;

    case 11: {
      size_t palette_start = 54;
      size_t palette_end = offbits;
      if (palette_end > palette_start && palette_end <= inout_sample->size) {
        size_t pos = palette_start + (prng->Rand() % (palette_end - palette_start));
        inout_sample->bytes[pos] = prng->Rand() & 0xff;
      }
      break;
    }
    case 12:
      RebuildValid8bppBmp(inout_sample, prng);
      break;
    
      case 13: {
      // v4.1: Strong bfOffBits mutation.
      // Goal: confuse pixel-data start calculation.
      uint32_t file_size = (uint32_t)inout_sample->size;
      uint32_t dib_size = ReadLE32(inout_sample, 0x0E);
      uint32_t header_end = 14 + dib_size;

      uint32_t candidates[] = {
        0,
        1,
        2,
        13,
        14,
        15,
        53,
        54,
        header_end,
        header_end > 0 ? header_end - 1 : 0,
        header_end + 1,
        offbits,
        offbits > 0 ? offbits - 1 : 0,
        offbits + 1,
        file_size > 0 ? file_size - 1 : 0,
        file_size,
        file_size + 1,
        file_size + (uint32_t)prng->Rand(2, 256),
        0xffffffff
      };

      size_t n = sizeof(candidates) / sizeof(candidates[0]);
      WriteLE32(inout_sample, 0x0A, candidates[prng->Rand() % n]);
      break;
    }

    case 14: {
      // v4.1: Strong biSizeImage mutation.
      // Goal: break internal copy/fill size calculation.
      uint32_t file_size = (uint32_t)inout_sample->size;
      uint32_t pixel_bytes = 0;

      if (offbits < file_size) {
        pixel_bytes = file_size - offbits;
      }

      uint32_t candidates[] = {
        0,
        1,
        2,
        3,
        4,
        15,
        16,
        31,
        32,
        255,
        256,
        257,
        pixel_bytes,
        pixel_bytes > 0 ? pixel_bytes - 1 : 0,
        pixel_bytes + 1,
        pixel_bytes + (uint32_t)prng->Rand(16, 4096),
        file_size,
        file_size + (uint32_t)prng->Rand(16, 4096),
        0x7fffffff,
        0xffffffff
      };

      size_t n = sizeof(candidates) / sizeof(candidates[0]);
      WriteLE32(inout_sample, 0x22, candidates[prng->Rand() % n]);
      break;
    }

    case 15: {
      // v4.1: Strong bfSize mutation.
      // Goal: desync declared file size from real file size.
      uint32_t file_size = (uint32_t)inout_sample->size;

      uint32_t candidates[] = {
        0,
        1,
        2,
        14,
        54,
        offbits,
        offbits > 0 ? offbits - 1 : 0,
        offbits + 1,
        file_size,
        file_size > 0 ? file_size - 1 : 0,
        file_size + 1,
        file_size + (uint32_t)prng->Rand(16, 4096),
        0x7fffffff,
        0xffffffff
      };

      size_t n = sizeof(candidates) / sizeof(candidates[0]);
      WriteLE32(inout_sample, 0x02, candidates[prng->Rand() % n]);
      break;
    }

    case 16: {
      // v4.1: Width boundary mutation.
      // Goal: disturb stride and pixel-buffer size calculation.
      uint32_t candidates[] = {
        0,
        1,
        2,
        3,
        4,
        7,
        8,
        15,
        16,
        31,
        32,
        63,
        64,
        127,
        128,
        255,
        256,
        257,
        511,
        512,
        1023,
        1024,
        1025,
        0x7fffffff,
        0xffffffff
      };

      size_t n = sizeof(candidates) / sizeof(candidates[0]);
      WriteLE32(inout_sample, 0x12, candidates[prng->Rand() % n]);
      break;
    }

    case 17: {
      // v4.1: Height boundary mutation.
      // Goal: disturb top-down/bottom-up and total image size calculation.
      uint32_t candidates[] = {
        0,
        1,
        2,
        3,
        4,
        7,
        8,
        15,
        16,
        31,
        32,
        63,
        64,
        127,
        128,
        255,
        256,
        257,
        511,
        512,
        1023,
        1024,
        1025,
        0xffffffff, // -1
        0xfffffffe, // -2
        0xffffff00, // -256
        0x80000000
      };

      size_t n = sizeof(candidates) / sizeof(candidates[0]);
      WriteLE32(inout_sample, 0x16, candidates[prng->Rand() % n]);
      break;
    }

    case 18: {
      // v4.1: Bit-count mutation.
      // Goal: desync bpp from palette/pixel layout.
      uint16_t candidates[] = {
        0,
        1,
        2,
        4,
        8,
        15,
        16,
        24,
        31,
        32,
        48,
        64,
        0xffff
      };

      size_t n = sizeof(candidates) / sizeof(candidates[0]);
      WriteLE16(inout_sample, 0x1C, candidates[prng->Rand() % n]);
      break;
    }
  }

  return true;
}

bool BmpAwareMutator::RebuildValid8bppBmp(Sample *sample, PRNG *prng) {
  if (!IsLikelyBmp(sample)) return false;

  static const uint32_t dims[] = {
    1, 2, 3, 4, 7, 8, 15, 16, 31, 32,
    63, 64, 127, 128, 255, 256, 257
  };

  const size_t ndims = sizeof(dims) / sizeof(dims[0]);

  uint32_t width = dims[prng->Rand() % ndims];
  uint32_t height_abs = dims[prng->Rand() % ndims];

  bool top_down = prng->Rand(0, 9) == 0;
  uint32_t height_field = top_down ? (uint32_t)(0 - height_abs) : height_abs;

  uint32_t stride = (width + 3) & ~3U;
  uint32_t pixel_size = stride * height_abs;
  uint32_t palette_size = 256 * 4;
  uint32_t offbits = 54 + palette_size;
  uint32_t file_size = offbits + pixel_size;

  if (file_size > Sample::max_size) return false;
  if (file_size < offbits) return false;

  uint32_t old_offbits = ReadLE32(sample, 0x0A);
  std::vector<char> old_pixels;

  if (old_offbits < sample->size) {
    old_pixels.assign(sample->bytes + old_offbits, sample->bytes + sample->size);
  }

  sample->Resize(file_size);

  sample->bytes[0] = 'B';
  sample->bytes[1] = 'M';

  WriteLE32(sample, 0x02, file_size);
  WriteLE32(sample, 0x06, 0);
  WriteLE32(sample, 0x0A, offbits);

  WriteLE32(sample, 0x0E, 40);
  WriteLE32(sample, 0x12, width);
  WriteLE32(sample, 0x16, height_field);
  WriteLE16(sample, 0x1A, 1);
  WriteLE16(sample, 0x1C, 8);
  WriteLE32(sample, 0x1E, 0);
  WriteLE32(sample, 0x22, pixel_size);
  WriteLE32(sample, 0x26, 2835);
  WriteLE32(sample, 0x2A, 2835);
  WriteLE32(sample, 0x2E, 256);
  WriteLE32(sample, 0x32, 0);

  for (uint32_t i = 0; i < 256; i++) {
    size_t p = 54 + i * 4;
    sample->bytes[p + 0] = (char)i;
    sample->bytes[p + 1] = (char)(255 - i);
    sample->bytes[p + 2] = (char)(prng->Rand() & 0xff);
    sample->bytes[p + 3] = 0;
  }

  for (uint32_t i = 0; i < pixel_size; i++) {
    if (!old_pixels.empty()) {
      sample->bytes[offbits + i] = old_pixels[i % old_pixels.size()];
    } else {
      sample->bytes[offbits + i] = (char)(prng->Rand() & 0xff);
    }
  }

  switch (prng->Rand(0, 7)) {
    case 0:
      WriteLE32(sample, 0x22, pixel_size + prng->Rand(1, 16));
      break;

    case 1:
      WriteLE32(sample, 0x22, pixel_size - prng->Rand(0, pixel_size > 16 ? 16 : pixel_size));
      break;

    case 2:
      WriteLE32(sample, 0x02, file_size + prng->Rand(1, 32));
      break;

    case 3:
      WriteLE32(sample, 0x0A, offbits - 4);
      break;

    case 4:
      WriteLE32(sample, 0x2E, prng->Rand(0, 300));
      break;

    case 5:
      if (width == 255 || width == 256 || width == 257) {
        WriteLE32(sample, 0x12, width + prng->Rand(0, 2));
      }
      break;

    case 6:
      if (height_abs == 255 || height_abs == 256 || height_abs == 257) {
        WriteLE32(sample, 0x16, height_field + prng->Rand(0, 2));
      }
      break;

    case 7:
      WriteLE32(sample, 0x1E, prng->Rand(0, 3));
      break;
  }

  return true;
}