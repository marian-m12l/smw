#pragma once
#include "types.h"
#include <string.h>
#include <stdio.h>
#include "snes/snes_regs.h"

extern uint8 bug_fix_counter;
extern uint8 g_ram[0x20000];
extern uint8 g_byte_700000[0x2000];

extern void RtlApuWrite(uint32 adr, uint8 val);
extern int snes_frame_counter;

#define RTL_SRAM_SIZE 0x800
extern uint8 *g_sram;
extern const uint8 *g_rom;
extern bool g_use_my_apu_code;
#define LONGPTR(t) {(t) & 0xffff, (t) >> 16}
extern bool g_debug_flag;

struct LongPtr;
void mov24(LongPtr *dst, uint32 src);
uint32 Load24(const LongPtr *src);
void MemCpy(void *dst, const void *src, int size);
bool Unreachable();

#if defined(_DEBUG)
// Gives better warning messages but non inlined on tcc
static inline uint16 GET_WORD(const uint8 *p) { return *(uint16 *)(p); }
static inline const uint8 *RomFixedPtr(uint32_t addr) { return &g_rom[(((addr >> 16) << 15) | (addr & 0x7fff)) & 0x3fffff]; }
#else
#define GET_WORD(p) (*(uint16*)(p))
#define RomFixedPtr(addr) (&g_rom[(((addr >> 16) << 15) | (addr & 0x7fff)) & 0x3fffff])
#endif

#define GET_BYTE(p) (*(uint8*)(p))

uint8 *RomPtr(uint32_t addr);
static inline uint8 *RomPtr_RAM(uint16_t addr) { assert(addr < 0x2000); return g_ram + addr; }
static inline uint8 *RomPtr_00(uint16_t addr) { return RomPtr(0x000000 | addr); }
static inline uint8 *RomPtr_01(uint16_t addr) { return RomPtr(0x010000 | addr); }
static inline uint8 *RomPtr_02(uint16_t addr) { return RomPtr(0x020000 | addr); }
static inline uint8 *RomPtr_03(uint16_t addr) { return RomPtr(0x030000 | addr); }
static inline uint8 *RomPtr_04(uint16_t addr) { return RomPtr(0x040000 | addr); }
static inline uint8 *RomPtr_05(uint16_t addr) { return RomPtr(0x050000 | addr); }
static inline uint8 *RomPtr_06(uint16_t addr) { return RomPtr(0x060000 | addr); }
static inline uint8 *RomPtr_07(uint16_t addr) { return RomPtr(0x070000 | addr); }
static inline uint8 *RomPtr_08(uint16_t addr) { return RomPtr(0x080000 | addr); }
static inline uint8 *RomPtr_09(uint16_t addr) { return RomPtr(0x090000 | addr); }
static inline uint8 *RomPtr_0A(uint16_t addr) { return RomPtr(0x0a0000 | addr); }
static inline uint8 *RomPtr_0B(uint16_t addr) { return RomPtr(0x0b0000 | addr); }
static inline uint8 *RomPtr_0C(uint16_t addr) { return RomPtr(0x0c0000 | addr); }
static inline uint8 *RomPtr_0D(uint16_t addr) { return RomPtr(0x0d0000 | addr); }
static inline uint8 *RomPtr_0E(uint16_t addr) { return RomPtr(0x0e0000 | addr); }
static inline uint8 *RomPtr_0F(uint16_t addr) { return RomPtr(0x0f0000 | addr); }
static inline uint8 *RomPtrWithBank(uint8 bank, uint16_t addr) { return RomPtr((bank << 16) | addr); }

OamEnt *get_OamEnt(OamEnt *base, uint16 off);
PointU16 *get_PointU16(PointU16 *pt, uint8 off);

void WriteReg(uint16 reg, uint8 value);
void WriteRegWord(uint16 reg, uint16 value);
uint16 ReadRegWord(uint16 reg);
uint8 ReadReg(uint16 reg);
uint8_t *IndirPtr(void *ptr, uint16 offs);
void IndirWriteWord(void *ptr, uint16 offs, uint16 value);
void IndirWriteByte(void *ptr, uint16 offs, uint8 value);

typedef void RunFrameFunc(uint16 input, int run_what);
typedef void SyncAllFunc();

void RtlReset(int mode);
void RtlSetupEmuCallbacks(uint8 *emu_ram, RunFrameFunc *func, SyncAllFunc *sync_all);
void RtlClearKeyLog();
void RtlStopReplay();

enum {
  kSaveLoad_Save = 1,
  kSaveLoad_Load = 2,
  kSaveLoad_Replay = 3,
};

void RtlSaveLoad(int cmd, uint8* slot);
void RtlCheat(char c);
void RtlApuLock();
void RtlApuUnlock();
void RtlSetUploadingApu(bool uploading);
void RtlApuUpload(const uint8 *p);
void RtlRenderAudio(int16 *audio_buffer, int samples, int channels);
void RtlPushApuState();
bool RtlRunFrame(int inputs);
void RtlReadSram();
void RtlWriteSram();

extern uint8_t* readSramImpl();
extern void writeSramImpl(uint8_t* sram);

extern void writeSaveStateInitImpl();
extern void writeSaveStateImpl(uint8_t* data, size_t size);
extern void writeSaveStateFinalizeImpl();

void RtlSaveSnapshot(/*const char *filename, bool saving_with_bug*/ uint8* slot_addr);
void RtlUpdateSnesPatchForBugfix();
void RtlLevelFinishedHook();
extern uint16 currently_installed_bug_fix_counter;
extern bool g_did_finish_level_hook;

uint16 Mult8x8(uint8 a, uint8 b);
uint16 SnesDivide(uint16 a, uint8 b);
uint16 SnesModulus(uint16 a, uint8 b);
void AddHiLo(uint8 *hi, uint8 *lo, uint16 v);
void SetHiLo(uint8 *hi, uint8 *lo, uint16 v);
void AddSprXPos(uint8 k, uint16 x);
void AddSprYPos(uint8 k, uint16 y);
void AddSprXYPos(uint8 k, uint16 x, uint16 y);
uint16 GetSprXPos(uint8 k);
uint16 GetSprYPos(uint8 k);
void SetSprXPos(uint8 k, uint16 x);
void SetSprYPos(uint8 k, uint16 y);
void SetSprXYPos(uint8 k, uint16 x, uint16 y);



typedef struct SpcPlayer SpcPlayer;
extern SpcPlayer *g_spc_player;

enum {
  kJoypadL_A = 0x80,
  kJoypadL_X = 0x40,
  kJoypadL_L = 0x20,
  kJoypadL_R = 0x10,

  kJoypadH_B = 0x80,
  kJoypadH_Y = 0x40,
  kJoypadH_Select = 0x20,
  kJoypadH_Start = 0x10,

  kJoypadH_Up = 0x8,
  kJoypadH_Down = 0x4,
  kJoypadH_Left = 0x2,
  kJoypadH_Right = 0x1,

  kJoypadH_AnyDir = 0xf,
};

struct OamEnt;
struct VramWriteEntry;

#pragma pack (push, 1)
typedef struct OwExits {
  uint16 field_0;
  uint16 field_2;
  uint8 field_4;
} OwExits;

typedef struct SpriteSlotData {
  uint8 field_0;
  uint16 field_1;
  uint16 field_3;
} SpriteSlotData;

typedef struct LevelTileAnimations {
  uint16 field_0;
  uint16 field_2;
  uint16 field_4;
} LevelTileAnimations;

#pragma pack (pop)

PairU16 MakePairU16(uint16 k, uint16 j);
#define MakePairU16_AX MakePairU16
#define MakePairU16_AY MakePairU16

