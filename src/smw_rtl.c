#include "smw_rtl.h"
#include "smw_cpu_infra.h"
#include "types.h"
#include "variables.h"
#include "funcs.h"
#include "spc_player.h"
#include "util.h"
#include "config.h"
#include <time.h>

struct StateRecorder;

static void RtlSaveMusicStateToRam_Locked();
static void RtlRestoreMusicAfterLoad_Locked(bool is_reset);

uint8 g_ram[0x20000];
uint8 *g_sram;
const uint8 *g_rom;
bool g_is_uploading_apu;
bool g_did_finish_level_hook;

static uint8 *g_rtl_memory_ptr;
static RunFrameFunc *g_rtl_runframe;
static SyncAllFunc *g_rtl_syncall;

void RtlSetupEmuCallbacks(uint8 *emu_ram, RunFrameFunc *func, SyncAllFunc *sync_all) {
  g_rtl_memory_ptr = emu_ram;
  g_rtl_runframe = func;
  g_rtl_syncall = sync_all;
}

static void RtlSynchronizeWholeState(void) {
  if (g_rtl_syncall)
    g_rtl_syncall();
}

// |ptr| must be a pointer into g_ram, will synchronize the RAM memory with the
// emulator.
static void RtlSyncMemoryRegion(void *ptr, size_t n) {
  uint8 *data = (uint8 *)ptr;
  assert(data >= g_ram && data < g_ram + 0x20000);
  if (g_rtl_memory_ptr)
    memcpy(g_rtl_memory_ptr + (data - g_ram), data, n);
}

void ByteArray_AppendVl(ByteArray *arr, uint32 v) {
  for (; v >= 255; v -= 255)
    ByteArray_AppendByte(arr, 255);
  ByteArray_AppendByte(arr, v);
}

typedef struct SaveFuncState {
  SaveLoadInfo base;
  ByteArray array;
} SaveFuncState;

void saveFunc(SaveLoadInfo *sli, void *data, size_t data_size) {
  /*SaveFuncState *st = (SaveFuncState *)sli;
  ByteArray_AppendData(&st->array, (uint8*)data, data_size);*/
  writeSaveStateImpl(data, data_size);
}

typedef struct LoadFuncState {
  SaveLoadInfo base;
  uint8 *pstart, *p, *pend;
} LoadFuncState;

void loadFunc(SaveLoadInfo *sli, void *data, size_t data_size) {
  LoadFuncState *st = (LoadFuncState *)sli;
  assert((size_t)(st->pend - st->p) >= data_size);
  memcpy(data, st->p, data_size);
  st->p += data_size;
}
static void LoadSnesState(SaveLoadInfo *sli) {
  // Do the actual loading
  snes_saveload(g_snes, sli);

  g_snes->cpu->e = false;
  uint32 next = (g_snes->ram[g_snes->cpu->sp + 3] | g_snes->ram[g_snes->cpu->sp + 4] << 8 | g_snes->ram[g_snes->cpu->sp + 5] << 16) + 1;
  if (next == 0x82897e) {
    g_snes->ram[g_snes->cpu->sp + 3] = (0xF71B - 1) & 0xff;
    g_snes->ram[g_snes->cpu->sp + 4] = (0xF71B - 1) >> 8;
  }
  RtlSynchronizeWholeState();
}

static void SaveSnesState(SaveLoadInfo *sli) {
  snes_saveload(g_snes, sli);
}

static size_t InternalSaveLoadSize() {
  // FIXME savestate size ??? how much is saved by snes_saveload() ???
  // 275465 bytes
  return 27 + 0x10000 + 40 + 3024 + 15 + 192 + 66619 + 512 + 174 + 8192 + 58 + 0x20000 + 4;
}

typedef struct StateRecorder {
  uint16 last_inputs;
  uint32 frames_since_last;
  uint32 total_frames;

  // For replay
  uint32 replay_pos, replay_pos_last_complete;
  uint32 replay_frame_counter;
  uint32 replay_next_cmd_at;
  uint32 snapshot_flags;
  uint8 replay_cmd;
  bool replay_mode;

  ByteArray log;
  ByteArray base_snapshot;
} StateRecorder;

static StateRecorder state_recorder;

void StateRecorder_Init(StateRecorder *sr) {
  ByteArray_Destroy(&sr->log);
  ByteArray_Destroy(&sr->base_snapshot);
  memset(sr, 0, sizeof(*sr));
}

void StateRecorder_RecordCmd(StateRecorder *sr, uint8 cmd) {
  int frames = sr->frames_since_last;
  sr->frames_since_last = 0;
  int x = (cmd < 0xc0) ? 0xf : 0x1;
  ByteArray_AppendByte(&sr->log, cmd | (frames < x ? frames : x));
  if (frames >= x)
    ByteArray_AppendVl(&sr->log, frames - x);
}

void StateRecorder_Record(StateRecorder *sr, uint16 inputs) {
  uint16 diff = inputs ^ sr->last_inputs;
  if (diff != 0) {
    sr->last_inputs = inputs;
    //    printf("0x%.4x %d: ", diff, sr->frames_since_last);
    //    size_t lb = sr->log.size;
    for (int i = 0; i < 12; i++) {
      if ((diff >> i) & 1)
        StateRecorder_RecordCmd(sr, i << 4);
    }
    //    while (lb < sr->log.size)
    //      printf("%.2x ", sr->log.data[lb++]);
    //    printf("\n");
  }
  sr->frames_since_last++;
  sr->total_frames++;
}

void StateRecorder_RecordPatchByte(StateRecorder *sr, uint32 addr, const uint8 *value, int num) {
  assert(addr < 0x20000);

//  printf("%d: PatchByte(0x%x, 0x%x. %d): ", sr->frames_since_last, addr, *value, num);
  size_t lb = sr->log.size;
  int lq = (num - 1) <= 3 ? (num - 1) : 3;
  StateRecorder_RecordCmd(sr, 0xc0 | (addr & 0x10000 ? 2 : 0) | lq << 2);
  if (lq == 3)
    ByteArray_AppendVl(&sr->log, num - 1 - 3);
  ByteArray_AppendByte(&sr->log, addr >> 8);
  ByteArray_AppendByte(&sr->log, addr);
  for (int i = 0; i < num; i++)
    ByteArray_AppendByte(&sr->log, value[i]);
//    while (lb < sr->log.size)
//      printf("%.2x ", sr->log.data[lb++]);
//    printf("\n");
}

void ReadFromFile(FILE *f, void *data, size_t n) {
  if (fread(data, 1, n, f) != n)
    Die("fread failed\n");
}

void RtlReset(int mode) {
  // FIXME snes_frame_counter = 0;
  snes_reset(g_snes, true);
  if (!(mode & 1))
    memset(g_sram, 0, RTL_SRAM_SIZE);
  
  RtlApuLock();
  RtlRestoreMusicAfterLoad_Locked(true);
  RtlApuUnlock();

  RtlSynchronizeWholeState();

  if ((mode & 2) == 0)
    StateRecorder_Init(&state_recorder);
}

int GetFileSize(FILE *f) {
  fseek(f, 0, SEEK_END);
  int r = ftell(f);
  fseek(f, 0, SEEK_SET);
  return r;
}

/*void StateRecorder_Load(StateRecorder *sr, FILE *f, bool replay_mode) {
  uint32 hdr[16] = { 0 };

  bool is_old = false;
  bool is_reset = false;

  ReadFromFile(f, hdr, 8 * sizeof(uint32));
  if (hdr[0] != 2) {
    hdr[8] = hdr[7];
    hdr[7] = hdr[5] >> 1;
    hdr[5] = (hdr[5] & 1) ? hdr[6] : 0;
  } else if (hdr[0] == 2) {
    ReadFromFile(f, hdr + 8, 8 * sizeof(uint32));

  } else {
    assert(0);
  }

  sr->total_frames = hdr[1];
  ByteArray_Resize(&sr->log, hdr[2]);
  ReadFromFile(f, sr->log.data, sr->log.size);
  sr->last_inputs = hdr[3];
  sr->frames_since_last = hdr[4];

  ByteArray_Resize(&sr->base_snapshot, hdr[5]);
  ReadFromFile(f, sr->base_snapshot.data, sr->base_snapshot.size);

  sr->snapshot_flags = hdr[9];
  sr->replay_next_cmd_at = 0;
  sr->replay_mode = replay_mode;
  if (replay_mode) {
    sr->frames_since_last = 0;
    sr->last_inputs = 0;
    sr->replay_pos = sr->replay_pos_last_complete = 0;
    sr->replay_frame_counter = 0;
    // Load snapshot from |base_snapshot_|, or reset if empty.
    if (sr->base_snapshot.size > 8192 ) {
      LoadFuncState state = { { &loadFunc }, sr->base_snapshot.data, sr->base_snapshot.data, sr->base_snapshot.data + sr->base_snapshot.size };
      LoadSnesState(&state.base);
      assert(state.p == state.pend);
    } else {
      RtlReset(2);
      if (sr->base_snapshot.size == RTL_SRAM_SIZE)
        memcpy(g_sram, sr->base_snapshot.data, RTL_SRAM_SIZE);
      is_reset = true;
    }
  } else {
    // Resume replay from the saved position?
    sr->replay_pos = sr->replay_pos_last_complete = hdr[7];
    sr->replay_frame_counter = hdr[8];
    sr->replay_mode = (sr->replay_frame_counter != 0);

//    assert(hdr[6] == 269349);
    ByteArray arr = { 0 };
    ByteArray_Resize(&arr, hdr[6]);
    ReadFromFile(f, arr.data, arr.size);

    if (hdr[6] == 269349) {
      // In the snapshot that's 269349 bytes big, the cart RAM is at 0x213eb
      printf("Warning. Old snapshot not supported! Reading only SRAM!\n");
      memcpy(g_snes->cart->ram, arr.data + 0x213eb, RTL_SRAM_SIZE);
    } else {
      LoadFuncState state = { {&loadFunc }, arr.data, arr.data, arr.data + arr.size };
      LoadSnesState(&state.base);
      assert(state.p == state.pend);
    }

    ByteArray_Destroy(&arr);

    if (is_old)
      RtlClearKeyLog();
  }

  if (!is_reset)
    RtlRestoreMusicAfterLoad_Locked(false);

  RtlUpdateSnesPatchForBugfix();

  // Temporarily fix reset state
//  if (g_snes->cpu->k == 0x82 && g_snes->cpu->pc == 0xf716)
//    g_snes->cpu->pc = 0xf71c;
}

void StateRecorder_Save(StateRecorder *sr, FILE *f, bool saving_with_bug) {
  uint32 hdr[16] = { 0 };
  SaveFuncState savest = { {&saveFunc} };
  SaveSnesState(&savest.base);
  assert(sr->base_snapshot.size == 0 || 
         sr->base_snapshot.size == savest.array.size || sr->base_snapshot.size == RTL_SRAM_SIZE);

  hdr[0] = 2;
  hdr[1] = sr->total_frames;
  hdr[2] = (uint32)sr->log.size;
  hdr[3] = sr->last_inputs;
  hdr[4] = sr->frames_since_last;
  hdr[5] = (uint32)sr->base_snapshot.size;
  hdr[6] = (uint32)savest.array.size;
  // If saving while in replay mode, also need to persist
  // sr->replay_pos_last_complete and sr->replay_frame_counter
  // so the replaying can be resumed.
  if (sr->replay_mode) {
    hdr[7] = sr->replay_pos_last_complete;
    hdr[8] = sr->replay_frame_counter;
  }
  hdr[9] = saving_with_bug * 1;
  fwrite(hdr, 1, sizeof(hdr), f);
  fwrite(sr->log.data, 1, sr->log.size, f);
  fwrite(sr->base_snapshot.data, 1, sr->base_snapshot.size, f);
  fwrite(savest.array.data, 1, savest.array.size, f);

  ByteArray_Destroy(&savest.array);
}

void StateRecorder_ClearKeyLog(StateRecorder *sr) {
  printf("Clearing key log!\n");
  SaveFuncState savest = { {&saveFunc} };
  savest.array = sr->base_snapshot;
  savest.array.size = 0;
  SaveSnesState(&savest.base);
  sr->base_snapshot = savest.array;

  ByteArray old_log = sr->log;
  int old_frames_since_last = sr->frames_since_last;
  memset(&sr->log, 0, sizeof(sr->log));
  // If there are currently any active inputs, record them initially at timestamp 0.
  sr->frames_since_last = 0;
  if (sr->last_inputs) {
    for (int i = 0; i < 12; i++) {
      if ((sr->last_inputs >> i) & 1)
        StateRecorder_RecordCmd(sr, i << 4);
    }
  }
  if (sr->replay_mode) {
    // When clearing the key log while in replay mode, we want to keep
    // replaying but discarding all key history up until this point.
    if (sr->replay_next_cmd_at != 0xffffffff) {
      sr->replay_next_cmd_at -= old_frames_since_last;
      sr->frames_since_last = sr->replay_next_cmd_at;
      sr->replay_pos_last_complete = (uint32)sr->log.size;
      StateRecorder_RecordCmd(sr, sr->replay_cmd);
      int old_replay_pos = sr->replay_pos;
      sr->replay_pos = (uint32)sr->log.size;
      ByteArray_AppendData(&sr->log, old_log.data + old_replay_pos, old_log.size - old_replay_pos);
    }
    sr->total_frames -= sr->replay_frame_counter;
    sr->replay_frame_counter = 0;
  } else {
    sr->total_frames = 0;
  }
  ByteArray_Destroy(&old_log);
  sr->frames_since_last = 0;
}

uint16 StateRecorder_ReadNextReplayState(StateRecorder *sr) {
  assert(sr->replay_mode);
  while (sr->frames_since_last >= sr->replay_next_cmd_at) {
    int replay_pos = sr->replay_pos;
    if (replay_pos != sr->replay_pos_last_complete) {
      // Apply next command
      sr->frames_since_last = 0;
      if (sr->replay_cmd < 0xc0) {
        sr->last_inputs ^= 1 << (sr->replay_cmd >> 4);
      } else if (sr->replay_cmd < 0xd0) {
        int nb = 1 + ((sr->replay_cmd >> 2) & 3);
        uint8 t;
        if (nb == 4) do {
          nb += t = sr->log.data[replay_pos++];
        } while (t == 255);
        uint32 addr = ((sr->replay_cmd >> 1) & 1) << 16;
        addr |= sr->log.data[replay_pos++] << 8;
        addr |= sr->log.data[replay_pos++];
        do {
          g_ram[addr & 0x1ffff] = sr->log.data[replay_pos++];
          RtlSyncMemoryRegion(&g_ram[addr & 0x1ffff], 1);
        } while (addr++, --nb);
      } else {
        assert(0);
      }
    }
    sr->replay_pos_last_complete = replay_pos;
    if (replay_pos >= sr->log.size) {
      sr->replay_pos = replay_pos;
      sr->replay_next_cmd_at = 0xffffffff;
      break;
    }
    // Read the next one
    uint8 cmd = sr->log.data[replay_pos++], t;
    int mask = (cmd < 0xc0) ? 0xf : 0x1;
    int frames = cmd & mask;
    if (frames == mask) do {
      frames += t = sr->log.data[replay_pos++];
    } while (t == 255);
    sr->replay_next_cmd_at = frames;
    sr->replay_cmd = cmd;
    sr->replay_pos = replay_pos;
  }
  sr->frames_since_last++;
  // Turn off replay mode after we reached the final frame position
  if (++sr->replay_frame_counter >= sr->total_frames) {
    sr->replay_mode = false;
  }
  return sr->last_inputs;
}

void StateRecorder_StopReplay(StateRecorder *sr) {
  if (!sr->replay_mode)
    return;
  sr->replay_mode = false;
  sr->total_frames = sr->replay_frame_counter;
  sr->log.size = sr->replay_pos_last_complete;
}*/

void StateRecorder_Load(uint8* slot_addr) {
  size_t size = *((size_t*) slot_addr);
  LoadFuncState state = { {&loadFunc }, slot_addr + sizeof(size_t), slot_addr + sizeof(size_t), slot_addr + sizeof(size_t) + size };
  LoadSnesState(&state.base);
  assert(state.p == state.pend);
}

void StateRecorder_Save(uint8* slot_addr) {
  size_t savestateSize = InternalSaveLoadSize();
  writeSaveStateInitImpl();
  writeSaveStateImpl(&savestateSize, sizeof(size_t));
  SaveFuncState savest = { {&saveFunc} };
  SaveSnesState(&savest.base);
  writeSaveStateFinalizeImpl();
}


void RtlClearKeyLog(void) {
  StateRecorder_ClearKeyLog(&state_recorder);
}

void RtlStopReplay(void) {
  StateRecorder_StopReplay(&state_recorder);
}

void RtlSavePlaythroughSnapshot() {
  /*char buf[128];
  snprintf(buf, sizeof(buf), "playthrough/%d_%d_%d.sav", ow_level_number_lo, misc_exit_level_action, (int)time(NULL));
  RtlSaveSnapshot(buf, false);
  StateRecorder_ClearKeyLog(&state_recorder);*/
}

enum {
  // Version was bumped to 1 after I fixed bug #1
  kCurrentBugFixCounter = 1,
};

bool RtlRunFrame(int inputs) {
  if (g_did_finish_level_hook) {
    if (!state_recorder.replay_mode && g_config.save_playthrough)
      RtlSavePlaythroughSnapshot();
    g_did_finish_level_hook = false;
  }

  // Avoid up/down and left/right from being pressed at the same time
  if ((inputs & 0x30) == 0x30) inputs ^= 0x30;
  if ((inputs & 0xc0) == 0xc0) inputs ^= 0xc0;

  bool is_replay = state_recorder.replay_mode;

  // Either copy state or apply state
  if (is_replay) {
    //inputs = StateRecorder_ReadNextReplayState(&state_recorder);
  } else {
    // Loading a bug snapshot?
    if (state_recorder.snapshot_flags & 1) {
      state_recorder.snapshot_flags &= ~1;
      inputs = state_recorder.last_inputs;
    } else {
      /*if (bug_fix_counter != kCurrentBugFixCounter) {
        if (g_debug_flag)
          printf("bug_fix_counter %d => %d\n", bug_fix_counter, kCurrentBugFixCounter);
        if (bug_fix_counter < kCurrentBugFixCounter) {
          bug_fix_counter = kCurrentBugFixCounter;
          //StateRecorder_RecordPatchByte(&state_recorder, (uint8 *)&bug_fix_counter - g_ram, (uint8 *)&bug_fix_counter, 2);
        }
      }*/
    }

    StateRecorder_Record(&state_recorder, inputs);
  }

  //if (bug_fix_counter != currently_installed_bug_fix_counter)
  //  RtlUpdateSnesPatchForBugfix();

  g_rtl_runframe(inputs, 0);

  // FIXME snes_frame_counter++;

  RtlPushApuState();
  return is_replay;
}

void RtlSaveSnapshot(/*const char *filename, bool saving_with_bug*/ uint8* slot_addr) {
  //FILE *f = fopen(filename, "wb");
  RtlApuLock();
  RtlSaveMusicStateToRam_Locked();
  StateRecorder_Save(slot_addr);
  RtlApuUnlock();
  //fclose(f);
}

static const char *const kBugSaves[] = {


"bug-1684462699",
"walking_on_moleman",
"slowfireballs",
"flying_fish",
"entering_this_area",
"dunno1",
"caping_behind_goal",
"bouncing_fire",
};

void RtlSaveLoad(int cmd, uint8* slot) {
  /*char name[128];
  if (slot >= 256) {
    int i = slot - 256;
    if (cmd == kSaveLoad_Save || i >= sizeof(kBugSaves) / sizeof(kBugSaves[0]))
      return;
    sprintf(name, "saves/%s.sav", kBugSaves[i]);
  } else {
    sprintf(name, "saves/save%d.sav", slot);
  }
  printf("*** %s slot %d\n",
    cmd == kSaveLoad_Save ? "Saving" : cmd == kSaveLoad_Load ? "Loading" : "Replaying", slot);*/

  if (cmd != kSaveLoad_Save) {

    /*FILE *f = fopen(name, "rb");
    if (f == NULL) {
      printf("Failed fopen: %s\n", name);
      return;
    }
    RtlApuLock();*/
    StateRecorder_Load(/*&state_recorder, f, cmd == kSaveLoad_Replay*/ slot);
    ppu_copy(g_snes->my_ppu, g_snes->ppu);
    RtlApuUnlock();
    RtlSynchronizeWholeState();
    //fclose(f);
  } else {
    RtlSaveSnapshot(/*name, false*/slot);
  }
}

void MemCpy(void *dst, const void *src, int size) {
  memcpy(dst, src, size);
}

PairU16 MakePairU16(uint16 k, uint16 j) {
  PairU16 r = { k, j };
  return r;
}

void mov24(struct LongPtr *a, uint32 d) {
  a->addr = d & 0xffff;
  a->bank = d >> 16;
}

uint32 Load24(const LongPtr *src) {
  return *(uint32 *)src & 0xffffff;
}

bool Unreachable(void) {
  printf("Unreachable!\n");
  assert(0);
  g_ram[0x1ffff] = 1;
  return false;
}

uint8 *RomPtr(uint32_t addr) {
  if (!(addr & 0x8000)) {
    printf("RomPtr - Invalid access 0x%x!\n", addr);
    if (!g_fail) {
      g_fail = true;
    }
  }
  return (uint8 *)&g_rom[(((addr >> 16) << 15) | (addr & 0x7fff)) & 0x3fffff];
}

void WriteReg(uint16 reg, uint8 value) {
  snes_write(g_snes, reg, value);
}

uint16 Mult8x8(uint8 a, uint8 b) {
  return a * b;
}

uint16 SnesDivide(uint16 a, uint8 b) {
  return (b == 0) ? 0xffff : a / b;
}

uint16 SnesModulus(uint16 a, uint8 b) {
  return (b == 0) ? a : a % b;
}

OamEnt *get_OamEnt(OamEnt *base, uint16 off) {
  return (OamEnt *)((uint8*)base + off);
}

PointU16 *get_PointU16(PointU16 *base, uint8 off) {
  return (PointU16 *)((uint8 *)base + off);
}


uint8 ReadReg(uint16 reg) {
  return snes_read(g_snes, reg);
}

uint16 ReadRegWord(uint16 reg) {
  uint16_t rv = ReadReg(reg);
  rv |= ReadReg(reg + 1) << 8;
  return rv;
}

void WriteRegWord(uint16 reg, uint16 value) {
  WriteReg(reg, (uint8)value);
  WriteReg(reg + 1, value >> 8);
}

uint8 *IndirPtr(void *ptr, uint16 offs) {
  uint32 a = (*(uint32 *)ptr & 0xffffff) + offs;
  if ((a >> 16) >= 0x7e && (a >> 16) <= 0x7f || a < 0x2000) {
    return &g_ram[a & 0x1ffff];
  } else {
    return RomPtr(a);
  }
}

void IndirWriteWord(void *ptr, uint16 offs, uint16 value) {
  uint8 *p = IndirPtr(ptr, offs);
  WORD(*p) = value;
}

void IndirWriteByte(void *ptr, uint16 offs, uint8 value) {
  uint8 *p = IndirPtr(ptr, offs);
  p[0] = value;
}

void SetHiLo(uint8 *hi, uint8 *lo, uint16 v) {
  *hi = v >> 8;
  *lo = v;
}

void AddHiLo(uint8 *hi, uint8 *lo, uint16 v) {
  SetHiLo(hi, lo, PAIR16(*hi, *lo) + v);
}

void AddSprXPos(uint8 k, uint16 x) {
  AddHiLo(&spr_xpos_hi[k], &spr_xpos_lo[k], x);
}

void AddSprYPos(uint8 k, uint16 y) {
  AddHiLo(&spr_ypos_hi[k], &spr_ypos_lo[k], y);
}

void AddSprXYPos(uint8 k, uint16 x, uint16 y) {
  AddHiLo(&spr_xpos_hi[k], &spr_xpos_lo[k], x);
  AddHiLo(&spr_ypos_hi[k], &spr_ypos_lo[k], y);
}

uint16 GetSprXPos(uint8 k) {
  return PAIR16(spr_xpos_hi[k], spr_xpos_lo[k]);
}

uint16 GetSprYPos(uint8 k) {
  return PAIR16(spr_ypos_hi[k], spr_ypos_lo[k]);
}

void SetSprXPos(uint8 k, uint16 x) {
  spr_xpos_hi[k] = x >> 8;
  spr_xpos_lo[k] = x;
}

void SetSprYPos(uint8 k, uint16 y) {
  spr_ypos_hi[k] = y >> 8;
  spr_ypos_lo[k] = y;
}

void SetSprXYPos(uint8 k, uint16 x, uint16 y) {
  SetHiLo(&spr_xpos_hi[k], &spr_xpos_lo[k], x);
  SetHiLo(&spr_ypos_hi[k], &spr_ypos_lo[k], y);
}

// Maintain a queue cause the snes and audio callback are not in sync.
// If an entry is 255, it means unset.
typedef struct ApuWriteEnt {
  uint8 ports[4];
} ApuWriteEnt;

enum {
  kApuMaxQueueSize = 16,
};

static struct ApuWriteEnt g_apu_write_ents[kApuMaxQueueSize], g_apu_write;
static uint8 g_apu_write_ent_pos, g_apu_queue_size, g_apu_time_since_empty;

void RtlSetUploadingApu(bool uploading) {
  RtlApuLock();
  if (g_is_uploading_apu != uploading) {
    printf("Uploading apu = %d\n", uploading);
    if (!uploading) {
      g_snes->apuCatchupCycles = 10000;
      snes_catchupApu(g_snes);
    } else {
      g_apu_queue_size = 0;
    }
    g_is_uploading_apu = uploading;
  }
  RtlApuUnlock();
}

void RtlApuWrite(uint32 adr, uint8 val) {
  assert(adr >= APUI00 && adr <= APUI03);

  if (g_is_uploading_apu) {
//    g_snes->apuCatchupCycles = 32;
    snes_catchupApu(g_snes); // catch up the apu before writing
    g_snes->apu->inPorts[adr & 0x3] = val;
    return;
  }

  if (g_snes->runningWhichVersion != 2) {
    g_apu_write.ports[adr & 0x3] = val;
  }
}

static bool IsFrameEmpty(ApuWriteEnt *w) {
  return (w->ports[0] == 0) && (w->ports[1] == 0) && (w->ports[2] == 0) && (w->ports[3] == 0);
}

void RtlPushApuState(void) {
  RtlApuLock();
  if (!g_is_uploading_apu) {
    // Strive for the queue to be empty.
    if (g_apu_queue_size == 0) {
        g_apu_time_since_empty = 0;
    } else {
      if (g_apu_time_since_empty >= 32 && IsFrameEmpty(&g_apu_write)) {
        g_apu_time_since_empty -= 4;
        RtlApuUnlock();
        return;
      }
      g_apu_time_since_empty++;
    }
    // Merge the two oldest to make space
    ApuWriteEnt *w0 = &g_apu_write_ents[g_apu_write_ent_pos++ & (kApuMaxQueueSize - 1)];
    if (g_apu_queue_size == kApuMaxQueueSize) {
      ApuWriteEnt *w1 = &g_apu_write_ents[g_apu_write_ent_pos & (kApuMaxQueueSize - 1)];
      for (int i = 0; i < 4; i++)
        if (w1->ports[i] == 0)
          w1->ports[i] = w0->ports[i];
    } else {
      g_apu_queue_size++;
    }
    *w0 = g_apu_write;
  } else {
    g_apu_queue_size = 0;
  }
  RtlApuUnlock();
}

static void RtlPopApuState_Locked(void) {
  uint8 *input_ports = /*g_use_my_apu_code ? g_spc_player->input_ports : */g_snes->apu->inPorts;
  if (g_apu_queue_size != 0) {
    ApuWriteEnt *w = &g_apu_write_ents[(g_apu_write_ent_pos - g_apu_queue_size--) & (kApuMaxQueueSize - 1)];
    for (int i = 0; i != 4; i++) {
      input_ports[i] = w->ports[i];
    }
  }
}

static void RtlResetApuQueue(void) {
  g_apu_write_ent_pos = g_apu_time_since_empty = g_apu_queue_size = 0;
  memset(g_apu_write.ports, 0, sizeof(g_apu_write.ports));
}

void RtlApuUpload(const uint8 *p) {
  RtlApuLock();
  RtlResetApuQueue();
  SpcPlayer_Upload(g_spc_player, p);
  RtlApuUnlock();
}

void RtlRestoreMusicAfterLoad_Locked(bool is_reset) {
  /*if (g_use_my_apu_code) {
    memcpy(g_spc_player->ram, g_snes->apu->ram, 65536);
    memcpy(g_spc_player->dsp->ram, g_snes->apu->dsp->ram, sizeof(Dsp) - offsetof(Dsp, ram));
    SpcPlayer_CopyVariablesFromRam(g_spc_player);
  }*/

  if (is_reset) {
    SpcPlayer_Initialize(g_spc_player);
  }

  RtlResetApuQueue();
}

void RtlSaveMusicStateToRam_Locked(void) {
  /*if (g_use_my_apu_code) {
    SpcPlayer *spc_player = g_spc_player;

    uint8 tmp[4];
    memcpy(tmp, spc_player->input_ports, 4);
    memcpy(spc_player->input_ports, g_apu_write.ports, 4);

    SpcPlayer_CopyVariablesToRam(g_spc_player);
    memcpy(g_snes->apu->dsp->ram, g_spc_player->dsp->ram, sizeof(Dsp) - offsetof(Dsp, ram));
    memcpy(g_snes->apu->ram, g_spc_player->ram, 65536);

    memcpy(spc_player->input_ports, tmp, 4);
  }*/
}

void RtlRenderAudio(int16 *audio_buffer, int samples, int channels) {
  assert(channels == 2);
  RtlApuLock();

  RtlPopApuState_Locked();

  //if (!g_use_my_apu_code) {
  //  if (!g_is_uploading_apu) {
      while (g_snes->apu->dsp->sampleOffset < 534)
        apu_cycle(g_snes->apu);
      dsp_getSamples(g_snes->apu->dsp, audio_buffer, samples);
  //  }
  //} else {
  //  SpcPlayer_GenerateSamples(g_spc_player);
  //  dsp_getSamples(g_spc_player->dsp, audio_buffer, samples);
  //}

  RtlApuUnlock();
}

void RtlCheat(char c) {
  if (c == 'w') {
  } else if (c == 'q') {
  } else if (c == 'q') {
  }
}

void RtlReadSram(void) {
  /*FILE *f = fopen("saves/smw.srm", "rb");
  if (f) {
    if (fread(g_sram, 1, RTL_SRAM_SIZE, f) != RTL_SRAM_SIZE)
      fprintf(stderr, "Error reading saves/smw.srm\n");
    fclose(f);*/
    uint8_t* sram = readSramImpl();
    memcpy(g_sram, sram, 8192);
    RtlSynchronizeWholeState();
    /*ByteArray_Resize(&state_recorder.base_snapshot, RTL_SRAM_SIZE);
    memcpy(state_recorder.base_snapshot.data, g_sram, RTL_SRAM_SIZE);
  }*/
}

void RtlWriteSram(void) {
  writeSramImpl(g_sram);
  /*rename("saves/smw.srm", "saves/smw.srm.bak");
  FILE *f = fopen("saves/smw.srm", "wb");
  if (f) {
    fwrite(g_sram, 1, RTL_SRAM_SIZE, f);
    fclose(f);
  } else {
    fprintf(stderr, "Unable to write saves/smw.srm\n");
  }*/
}
