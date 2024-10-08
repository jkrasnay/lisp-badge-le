/* Lisp Badge LE - uLisp 4.6
   David Johnson-Davies - www.technoblogy.com - 25th June 2024

   Licensed under the MIT license: https://opensource.org/licenses/MIT
*/

// Lisp Library
const char LispLibrary[] PROGMEM = "";

// Compile options

#define checkoverflow
// #define resetautorun
#define printfreespace
#define serialmonitor
// #define printgcs
// #define sdcardsupport
// #define lisplibrary
#define assemblerlist
#define extensions

// Includes

// #include "LispLibrary.h"
#include <avr/sleep.h>
#include <setjmp.h>
#include <SPI.h>
#include <limits.h>

#if defined(sdcardsupport)
#include <SD.h>
#define SDSIZE 172
#else
#define SDSIZE 0
#endif

// Platform specific settings

#define WORDALIGNED __attribute__((aligned (2)))
#define OBJECTALIGNED __attribute__((aligned (4)))
#define BUFFERSIZE 22                     /* longest builtin name + 1 */

#if defined(__AVR_AVR128DA48__) || defined(__AVR_AVR128DB48__)  /* LispBadge LE */
  #include <Flash.h>
  #define WORKSPACESIZE (2800-SDSIZE)     /* Objects (4*bytes) */
  #define FLASHWRITESIZE 16384            /* Bytes */
  #define CODESIZE 96                     /* Bytes <= 512 */
  #define STACKDIFF 320
  #define CPU_AVR128DX48
  #define LED_BUILTIN 7

#else
#error "Board not supported!"
#endif

// C Macros

#define nil                NULL
#define car(x)             (((object *) (x))->car)
#define cdr(x)             (((object *) (x))->cdr)

#define first(x)           car(x)
#define rest(x)            cdr(x)
#define second(x)          first(rest(x))
#define cddr(x)            cdr(cdr(x))
#define third(x)           first(cddr(x))

#define push(x, y)         ((y) = cons((x),(y)))
#define pop(y)             ((y) = cdr(y))

#define protect(y)         push((y), GCStack)
#define unprotect()        pop(GCStack)

#define integerp(x)        ((x) != NULL && (x)->type == NUMBER)
#define symbolp(x)         ((x) != NULL && (x)->type == SYMBOL)
#define stringp(x)         ((x) != NULL && (x)->type == STRING)
#define characterp(x)      ((x) != NULL && (x)->type == CHARACTER)
#define arrayp(x)          ((x) != NULL && (x)->type == ARRAY)
#define streamp(x)         ((x) != NULL && (x)->type == STREAM)

#define mark(x)            (car(x) = (object *)(((uintptr_t)(car(x))) | MARKBIT))
#define unmark(x)          (car(x) = (object *)(((uintptr_t)(car(x))) & ~MARKBIT))
#define marked(x)          ((((uintptr_t)(car(x))) & MARKBIT) != 0)
#define MARKBIT            1

#define setflag(x)         (Flags = Flags | 1<<(x))
#define clrflag(x)         (Flags = Flags & ~(1<<(x)))
#define tstflag(x)         (Flags & 1<<(x))

#define issp(x)            (x == ' ' || x == '\n' || x == '\r' || x == '\t')
#define isbr(x)            (x == ')' || x == '(' || x == '"' || x == '#')
#define longsymbolp(x)     (((x)->name & 0x03) == 0)
#define longnamep(x)       (((x) & 0x03) == 0)
#define arraysize(x)       (sizeof(x) / sizeof(x[0]))
#define stringifyX(x)      #x
#define stringify(x)       stringifyX(x)
#define PACKEDS            17600
#define BUILTINS           64000
#define ENDFUNCTIONS       1536

// Code marker stores start and end of code block (max 256 bytes)
#define startblock(x)      ((x->integer) & 0xFF)
#define endblock(x)        ((x->integer) >> 8 & 0xFF)

#define SDCARD_SS_PIN 10

#define SHIFT_KEY 33
#define META_KEY 13

// Constants

const int TRACEMAX = 3; // Number of traced functions
enum type { ZZERO=0, SYMBOL=2, CODE=4, NUMBER=6, STREAM=8, CHARACTER=10, ARRAY=12, STRING=14, PAIR=16 };  // ARRAY STRING and PAIR must be last
enum token { UNUSED, BRA, KET, QUO, DOT };
enum stream { SERIALSTREAM, I2CSTREAM, SPISTREAM, SDSTREAM, STRINGSTREAM };
enum fntypes_t { OTHER_FORMS, TAIL_FORMS, FUNCTIONS, SPECIAL_FORMS };

// Stream names used by printobject
const char serialstream[] PROGMEM = "serial";
const char i2cstream[] PROGMEM = "i2c";
const char spistream[] PROGMEM = "spi";
const char sdstream[] PROGMEM = "sd";
const char stringstream[] PROGMEM = "string";
PGM_P const streamname[] PROGMEM = {serialstream, i2cstream, spistream, sdstream, stringstream};

// Typedefs

typedef uint16_t symbol_t;
typedef uint16_t builtin_t;
typedef uint16_t chars_t;

typedef struct sobject {
  union {
    struct {
      sobject *car;
      sobject *cdr;
    };
    struct {
      unsigned int type;
      union {
        symbol_t name;
        int integer;
        chars_t chars; // For strings
      };
    };
  };
} object;

typedef object *(*fn_ptr_type)(object *, object *);
typedef void (*mapfun_t)(object *, object **);
typedef int (*intfn_ptr_type)(int w, int x, int y, int z);

typedef const struct {
  const char *string;
  fn_ptr_type fptr;
  uint8_t minmax;
  const char *doc;
} tbl_entry_t;

typedef int (*gfun_t)();
typedef void (*pfun_t)(char);

enum builtins: builtin_t { NIL, TEE, NOTHING, OPTIONAL, FEATURES, INITIALELEMENT, ELEMENTTYPE, TEST, BIT, AMPREST,
LAMBDA, LET, LETSTAR, CLOSURE, PSTAR, QUOTE, DEFUN, DEFVAR, DEFCODE, EQ, CAR, FIRST, CDR, REST, NTH, AREF,
CHAR, STRINGFN, PINMODE, DIGITALWRITE, ANALOGREAD, ANALOGREFERENCE, REGISTER, FORMAT,
 };

// Global variables

object Workspace[WORKSPACESIZE] OBJECTALIGNED;
#if defined(CODESIZE)
uint8_t MyCode[CODESIZE] WORDALIGNED; // Must be even
#endif

jmp_buf toplevel_handler;
jmp_buf *handler = &toplevel_handler;
unsigned int Freespace = 0;
object *Freelist;
unsigned int I2Ccount;
unsigned int TraceFn[TRACEMAX];
unsigned int TraceDepth[TRACEMAX];
builtin_t Context;

object *GlobalEnv;
object *GCStack = NULL;
object *GlobalString;
object *GlobalStringTail;
int GlobalStringIndex = 0;
uint8_t PrintCount = 0;
uint8_t BreakLevel = 0;
char LastChar = 0;
char LastPrint = 0;
uint16_t RandomSeed;

// Graphics
uint8_t Xpos, Ypos, Xor;

// Flags
enum flag { PRINTREADABLY, RETURNFLAG, ESCAPE, EXITEDITOR, LIBRARYLOADED, NOESC, NOECHO, MUFFLEERRORS };
volatile uint8_t Flags = 0b00001; // PRINTREADABLY set by default

// Forward references
object *tee;
void pfstring (PGM_P s, pfun_t pfun);

inline symbol_t twist (builtin_t x) {
  return (x<<2) | ((x & 0xC000)>>14);
}

inline builtin_t untwist (symbol_t x) {
  return (x>>2 & 0x3FFF) | ((x & 0x03)<<14);
}

// Error handling

void errorsub (symbol_t fname, PGM_P string) {
  pfl(pserial); pfstring(PSTR("Error: "), pserial);
  if (fname != sym(NIL)) {
    pserial('\'');
    psymbol(fname, pserial);
    pserial('\''); pserial(' ');
  }
  pfstring(string, pserial);
}

void errorend () { GCStack = NULL; longjmp(*handler, 1); }

void errorsym (symbol_t fname, PGM_P string, object *symbol) {
  if (!tstflag(MUFFLEERRORS)) {
    errorsub(fname, string);
    pserial(':'); pserial(' ');
    printobject(symbol, pserial);
    pln(pserial);
  }
  errorend();
}

void errorsym2 (symbol_t fname, PGM_P string) {
  if (!tstflag(MUFFLEERRORS)) {
    errorsub(fname, string);
    pln(pserial);
  }
  errorend();
}

void error (PGM_P string, object *symbol) {
  errorsym(sym(Context), string, symbol);
}

void error2 (PGM_P string) {
  errorsym2(sym(Context), string);
}

void formaterr (object *formatstr, PGM_P string, uint8_t p) {
  pln(pserial); indent(4, ' ', pserial); printstring(formatstr, pserial); pln(pserial);
  indent(p+5, ' ', pserial); pserial('^');
  error2(string);
  pln(pserial);
  GCStack = NULL;
  longjmp(*handler, 1);
}

// Save space as these are used multiple times
const char notanumber[] PROGMEM = "argument is not a number";
const char notaninteger[] PROGMEM = "argument is not an integer";
const char notastring[] PROGMEM = "argument is not a string";
const char notalist[] PROGMEM = "argument is not a list";
const char notasymbol[] PROGMEM = "argument is not a symbol";
const char notproper[] PROGMEM = "argument is not a proper list";
const char toomanyargs[] PROGMEM = "too many arguments";
const char toofewargs[] PROGMEM = "too few arguments";
const char noargument[] PROGMEM = "missing argument";
const char nostream[] PROGMEM = "missing stream argument";
const char overflow[] PROGMEM = "arithmetic overflow";
const char divisionbyzero[] PROGMEM = "division by zero";
const char indexnegative[] PROGMEM = "index can't be negative";
const char invalidarg[] PROGMEM = "invalid argument";
const char invalidkey[] PROGMEM = "invalid keyword";
const char illegalclause[] PROGMEM = "illegal clause";
const char invalidpin[] PROGMEM = "invalid pin";
const char oddargs[] PROGMEM = "odd number of arguments";
const char indexrange[] PROGMEM = "index out of range";
const char canttakecar[] PROGMEM = "can't take car";
const char canttakecdr[] PROGMEM = "can't take cdr";
const char unknownstreamtype[] PROGMEM = "unknown stream type";

// Set up workspace

void initworkspace () {
  Freelist = NULL;
  for (int i=WORKSPACESIZE-1; i>=0; i--) {
    object *obj = &Workspace[i];
    car(obj) = NULL;
    cdr(obj) = Freelist;
    Freelist = obj;
    Freespace++;
  }
}

object *myalloc () {
  if (Freespace == 0) { Context = NIL; error2(PSTR("no room")); }
  object *temp = Freelist;
  Freelist = cdr(Freelist);
  Freespace--;
  return temp;
}

inline void myfree (object *obj) {
  car(obj) = NULL;
  cdr(obj) = Freelist;
  Freelist = obj;
  Freespace++;
}

// Make each type of object

object *number (int n) {
  object *ptr = myalloc();
  ptr->type = NUMBER;
  ptr->integer = n;
  return ptr;
}

object *character (uint8_t c) {
  object *ptr = myalloc();
  ptr->type = CHARACTER;
  ptr->chars = c;
  return ptr;
}

object *cons (object *arg1, object *arg2) {
  object *ptr = myalloc();
  ptr->car = arg1;
  ptr->cdr = arg2;
  return ptr;
}

object *symbol (symbol_t name) {
  object *ptr = myalloc();
  ptr->type = SYMBOL;
  ptr->name = name;
  return ptr;
}

inline object *bsymbol (builtin_t name) {
  return intern(twist(name+BUILTINS));
}

object *codehead (int entry) {
  object *ptr = myalloc();
  ptr->type = CODE;
  ptr->integer = entry;
  return ptr;
}

object *intern (symbol_t name) {
  for (int i=0; i<WORKSPACESIZE; i++) {
    object *obj = &Workspace[i];
    if (obj->type == SYMBOL && obj->name == name) return obj;
  }
  return symbol(name);
}

bool eqsymbols (object *obj, char *buffer) {
  object *arg = cdr(obj);
  int i = 0;
  while (!(arg == NULL && buffer[i] == 0)) {
    if (arg == NULL || buffer[i] == 0) return false;
    chars_t test = 0; int shift = 8;
    for (int j=0; j<2; j++, i++) {
      if (buffer[i] == 0) break;
      test = test | buffer[i]<<shift;
      shift = shift - 8;
    }
    if (arg->chars != test) return false;
    arg = car(arg);
  }
  return true;
}

object *internlong (char *buffer) {
  for (int i=0; i<WORKSPACESIZE; i++) {
    object *obj = &Workspace[i];
    if (obj->type == SYMBOL && longsymbolp(obj) && eqsymbols(obj, buffer)) return obj;
  }
  object *obj = lispstring(buffer);
  obj->type = SYMBOL;
  return obj;
}

object *stream (uint8_t streamtype, uint8_t address) {
  object *ptr = myalloc();
  ptr->type = STREAM;
  ptr->integer = streamtype<<8 | address;
  return ptr;
}

object *newstring () {
  object *ptr = myalloc();
  ptr->type = STRING;
  ptr->chars = 0;
  return ptr;
}

// Features

const char arrays[] PROGMEM = ":arrays";
const char doc[] PROGMEM = ":documentation";
const char machinecode[] PROGMEM = ":machine-code";
const char errorhandling[] PROGMEM = ":error-handling";

char *copyprogmemstring (PGM_P s, char *buffer) {
  int max = BUFFERSIZE-1;
  int i = 0;
  do {
    char c = pgm_read_byte(s++);
    buffer[i++] = c;
    if (c == 0) break;
  } while (i<max);
  return buffer;
}

object *features () {
  char buffer[BUFFERSIZE];
  object *result = NULL;
  push(internlong(copyprogmemstring(errorhandling, buffer)), result);
  #if defined(CODESIZE)
  push(internlong(copyprogmemstring(machinecode, buffer)), result);
  #endif
  push(internlong(copyprogmemstring(doc, buffer)), result);
  push(internlong(copyprogmemstring(arrays, buffer)), result);
  return result;
}

// Garbage collection

void markobject (object *obj) {
  MARK:
  if (obj == NULL) return;
  if (marked(obj)) return;

  object* arg = car(obj);
  unsigned int type = obj->type;
  mark(obj);

  if (type >= PAIR || type == ZZERO) { // cons
    markobject(arg);
    obj = cdr(obj);
    goto MARK;
  }

  if (type == ARRAY) {
    obj = cdr(obj);
    goto MARK;
  }

  if ((type == STRING) || (type == SYMBOL && longsymbolp(obj))) {
    obj = cdr(obj);
    while (obj != NULL) {
      arg = car(obj);
      mark(obj);
      obj = arg;
    }
  }
}

void sweep () {
  Freelist = NULL;
  Freespace = 0;
  for (int i=WORKSPACESIZE-1; i>=0; i--) {
    object *obj = &Workspace[i];
    if (!marked(obj)) myfree(obj); else unmark(obj);
  }
}

void gc (object *form, object *env) {
  #if defined(printgcs)
  int start = Freespace;
  #endif
  markobject(tee);
  markobject(GlobalEnv);
  markobject(GCStack);
  markobject(form);
  markobject(env);
  sweep();
  #if defined(printgcs)
  pfl(pserial); pserial('{'); pint(Freespace - start, pserial); pserial('}');
  #endif
}

// Compact image

void movepointer (object *from, object *to) {
  for (int i=0; i<WORKSPACESIZE; i++) {
    object *obj = &Workspace[i];
    unsigned int type = (obj->type) & ~MARKBIT;
    if (marked(obj) && (type >= ARRAY || type==ZZERO || (type == SYMBOL && longsymbolp(obj)))) {
      if (car(obj) == (object *)((uintptr_t)from | MARKBIT))
        car(obj) = (object *)((uintptr_t)to | MARKBIT);
      if (cdr(obj) == from) cdr(obj) = to;
    }
  }
  // Fix strings and long symbols
  for (int i=0; i<WORKSPACESIZE; i++) {
    object *obj = &Workspace[i];
    if (marked(obj)) {
      unsigned int type = (obj->type) & ~MARKBIT;
      if (type == STRING || (type == SYMBOL && longsymbolp(obj))) {
        obj = cdr(obj);
        while (obj != NULL) {
          if (cdr(obj) == to) cdr(obj) = from;
          obj = (object *)((uintptr_t)(car(obj)) & ~MARKBIT);
        }
      }
    }
  }
}

uintptr_t compactimage (object **arg) {
  markobject(tee);
  markobject(GlobalEnv);
  markobject(GCStack);
  object *firstfree = Workspace;
  while (marked(firstfree)) firstfree++;
  object *obj = &Workspace[WORKSPACESIZE-1];
  while (firstfree < obj) {
    if (marked(obj)) {
      car(firstfree) = car(obj);
      cdr(firstfree) = cdr(obj);
      unmark(obj);
      movepointer(obj, firstfree);
      if (GlobalEnv == obj) GlobalEnv = firstfree;
      if (GCStack == obj) GCStack = firstfree;
      if (*arg == obj) *arg = firstfree;
      while (marked(firstfree)) firstfree++;
    }
    obj--;
  }
  sweep();
  return firstfree - Workspace;
}

// Make SD card filename

char *MakeFilename (object *arg, char *buffer) {
  int max = BUFFERSIZE-1;
  int i = 0;
  do {
    char c = nthchar(arg, i);
    if (c == '\0') break;
    buffer[i++] = c;
  } while (i<max);
  buffer[i] = '\0';
  return buffer;
}

// Save-image and load-image

#if defined(sdcardsupport)
void SDWriteInt (File file, int data) {
  file.write(data & 0xFF); file.write(data>>8 & 0xFF);
}

int SDReadInt (File file) {
  uint8_t b0 = file.read(); uint8_t b1 = file.read();
  return b0 | b1<<8;
}
#elif defined(FLASHWRITESIZE)
#if defined (CPU_ATmega1284P)
// save-image area is the 16K bytes (64 256-byte pages) from 0x1bc00 to 0x1fc00
const uint32_t BaseAddress = 0x1bc00;
uint8_t FlashCheck() {
  return 0;
}

void FlashWriteInt (uint32_t *addr, int data) {
  if (((*addr) & 0xFF) == 0) optiboot_page_erase(BaseAddress + ((*addr) & 0xFF00));
  optiboot_page_fill(BaseAddress + *addr, data);
  if (((*addr) & 0xFF) == 0xFE) optiboot_page_write(BaseAddress + ((*addr) & 0xFF00));
  (*addr)++; (*addr)++;
}

void FlashEndWrite (uint32_t *addr) {
  if (((*addr) & 0xFF) != 0) optiboot_page_write((BaseAddress + ((*addr) & 0xFF00)));
}

uint8_t FlashReadByte (uint32_t *addr) {
  return pgm_read_byte_far(BaseAddress + (*addr)++);
}

int FlashReadInt (uint32_t *addr) {
  int data = pgm_read_word_far(BaseAddress + *addr);
  (*addr)++; (*addr)++;
  return data;
}
#elif defined (CPU_AVR128DX48)
// save-image area is the 16K bytes (32 512-byte pages) from 0x1c000 to 0x20000
const uint32_t BaseAddress = 0x1c000;
uint8_t FlashCheck() {
  return Flash.checkWritable();
}

void FlashWriteInt (uint32_t *addr, int data) {
  if (((*addr) & 0x1FF) == 0) Flash.erasePage(BaseAddress + ((*addr) & 0xFE00));
  Flash.writeWord(BaseAddress + *addr, data);
  (*addr)++; (*addr)++;
}

void FlashEndWrite (uint32_t *addr) {
  (void) addr;
}

uint8_t FlashReadByte (uint32_t *addr) {
  return Flash.readByte(BaseAddress + (*addr)++);
}

int FlashReadInt (uint32_t *addr) {
  int data = Flash.readWord(BaseAddress + *addr);
  (*addr)++; (*addr)++;
  return data;
}
#endif
#else
void EEPROMWriteInt (unsigned int *addr, int data) {
  EEPROM.write((*addr)++, data & 0xFF); EEPROM.write((*addr)++, data>>8 & 0xFF);
}

int EEPROMReadInt (unsigned int *addr) {
  uint8_t b0 = EEPROM.read((*addr)++); uint8_t b1 = EEPROM.read((*addr)++);
  return b0 | b1<<8;
}
#endif

unsigned int saveimage (object *arg) {
#if defined(sdcardsupport)
  unsigned int imagesize = compactimage(&arg);
  SD.begin(SDCARD_SS_PIN);
  File file;
  if (stringp(arg)) {
    char buffer[BUFFERSIZE];
    file = SD.open(MakeFilename(arg, buffer), O_RDWR | O_CREAT | O_TRUNC);
    if (!file) error2(PSTR("problem saving to SD card or invalid filename"));
    arg = NULL;
  } else if (arg == NULL || listp(arg)) {
    file = SD.open("/ULISP.IMG", O_RDWR | O_CREAT | O_TRUNC);
    if (!file) error2(PSTR("problem saving to SD card"));
  }
  else error(invalidarg, arg);
  SDWriteInt(file, (uintptr_t)arg);
  SDWriteInt(file, imagesize);
  SDWriteInt(file, (uintptr_t)GlobalEnv);
  SDWriteInt(file, (uintptr_t)GCStack);
  #if defined(CODESIZE)
  for (int i=0; i<CODESIZE; i++) file.write(MyCode[i]);
  #endif
  for (unsigned int i=0; i<imagesize; i++) {
    object *obj = &Workspace[i];
    SDWriteInt(file, (uintptr_t)car(obj));
    SDWriteInt(file, (uintptr_t)cdr(obj));
  }
  file.close();
  return imagesize;
#elif defined(FLASHWRITESIZE)
  unsigned int imagesize = compactimage(&arg);
  if (!(arg == NULL || listp(arg))) error(invalidarg, arg);
  if (FlashCheck()) error(PSTR("flash write not supported"), number(FlashCheck()));
  // Save to Flash
  #if defined(CODESIZE)
  int bytesneeded = 10 + CODESIZE + imagesize*4;
  #else
  int bytesneeded = 10 + imagesize*4;
  #endif
  if (bytesneeded > FLASHWRITESIZE) error(PSTR("image too large"), number(imagesize));
  uint32_t addr = 0;
  FlashWriteInt(&addr, (uintptr_t)arg);
  FlashWriteInt(&addr, imagesize);
  FlashWriteInt(&addr, (uintptr_t)GlobalEnv);
  FlashWriteInt(&addr, (uintptr_t)GCStack);
  #if defined(CODESIZE)
  for (int i=0; i<CODESIZE/2; i++) FlashWriteInt(&addr, MyCode[i*2] | MyCode[i*2+1]<<8);
  #endif
  for (unsigned int i=0; i<imagesize; i++) {
    object *obj = &Workspace[i];
    FlashWriteInt(&addr, (uintptr_t)car(obj));
    FlashWriteInt(&addr, (uintptr_t)cdr(obj));
  }
  FlashEndWrite(&addr);
  return imagesize;
#elif defined(EEPROMSIZE)
  unsigned int imagesize = compactimage(&arg);
  if (!(arg == NULL || listp(arg))) error(invalidarg, arg);
  int bytesneeded = imagesize*4 + 10;
  if (bytesneeded > EEPROMSIZE) error(PSTR("image too large"), number(imagesize));
  unsigned int addr = 0;
  EEPROMWriteInt(&addr, (unsigned int)arg);
  EEPROMWriteInt(&addr, imagesize);
  EEPROMWriteInt(&addr, (unsigned int)GlobalEnv);
  EEPROMWriteInt(&addr, (unsigned int)GCStack);
  for (unsigned int i=0; i<imagesize; i++) {
    object *obj = &Workspace[i];
    EEPROMWriteInt(&addr, (uintptr_t)car(obj));
    EEPROMWriteInt(&addr, (uintptr_t)cdr(obj));
  }
  return imagesize;
#else
  (void) arg;
  error2(PSTR("not available"));
  return 0;
#endif
}

unsigned int loadimage (object *arg) {
#if defined(sdcardsupport)
  SD.begin(SDCARD_SS_PIN);
  File file;
  if (stringp(arg)) {
    char buffer[BUFFERSIZE];
    file = SD.open(MakeFilename(arg, buffer));
    if (!file) error2(PSTR("problem loading from SD card or invalid filename"));
  }
  else if (arg == NULL) {
    file = SD.open("/ULISP.IMG");
    if (!file) error2(PSTR("problem loading from SD card"));
  }
  else error(invalidarg, arg);
  SDReadInt(file);
  unsigned int imagesize = SDReadInt(file);
  GlobalEnv = (object *)SDReadInt(file);
  GCStack = (object *)SDReadInt(file);
  #if defined(CODESIZE)
  for (int i=0; i<CODESIZE; i++) MyCode[i] = file.read();
  #endif
  for (unsigned int i=0; i<imagesize; i++) {
    object *obj = &Workspace[i];
    car(obj) = (object *)SDReadInt(file);
    cdr(obj) = (object *)SDReadInt(file);
  }
  file.close();
  gc(NULL, NULL);
  return imagesize;
#elif defined(FLASHWRITESIZE)
  (void) arg;
  if (FlashCheck()) error(PSTR("flash write not supported"), number(FlashCheck()));
  uint32_t addr = 0;
  FlashReadInt(&addr); // Skip eval address
  unsigned int imagesize = FlashReadInt(&addr);
  if (imagesize == 0 || imagesize == 0xFFFF) error2(PSTR("no saved image"));
  GlobalEnv = (object *)FlashReadInt(&addr);
  GCStack = (object *)FlashReadInt(&addr);
  #if defined(CODESIZE)
  for (int i=0; i<CODESIZE; i++) MyCode[i] = FlashReadByte(&addr);
  #endif
  for (unsigned int i=0; i<imagesize; i++) {
    object *obj = &Workspace[i];
    car(obj) = (object *)FlashReadInt(&addr);
    cdr(obj) = (object *)FlashReadInt(&addr);
  }
  gc(NULL, NULL);
  return imagesize;
#elif defined(EEPROMSIZE)
  (void) arg;
  unsigned int addr = 2; // Skip eval address
  unsigned int imagesize = EEPROMReadInt(&addr);
  if (imagesize == 0 || imagesize == 0xFFFF) error2(PSTR("no saved image"));
  GlobalEnv = (object *)EEPROMReadInt(&addr);
  GCStack = (object *)EEPROMReadInt(&addr);
  for (unsigned int i=0; i<imagesize; i++) {
    object *obj = &Workspace[i];
    car(obj) = (object *)EEPROMReadInt(&addr);
    cdr(obj) = (object *)EEPROMReadInt(&addr);
  }
  gc(NULL, NULL);
  return imagesize;
#else
  (void) arg;
  error2(PSTR("not available"));
  return 0;
#endif
}

void autorunimage () {
#if defined(sdcardsupport)
  SD.begin(SDCARD_SS_PIN);
  File file = SD.open("ULISP.IMG");
  if (!file) error2(PSTR("problem autorunning from SD card"));
  object *autorun = (object *)SDReadInt(file);
  file.close();
  if (autorun != NULL) {
    loadimage(NULL);
    apply(autorun, NULL, NULL);
  }
#elif defined(FLASHWRITESIZE)
  uint32_t addr = 0;
  object *autorun = (object *)FlashReadInt(&addr);
  if (autorun != NULL && (unsigned int)autorun != 0xFFFF) {
    loadimage(nil);
    apply(autorun, NULL, NULL);
  }
#elif defined(EEPROMSIZE)
  unsigned int addr = 0;
  object *autorun = (object *)EEPROMReadInt(&addr);
  if (autorun != NULL && (unsigned int)autorun != 0xFFFF) {
    loadimage(nil);
    apply(autorun, NULL, NULL);
  }
#else
  error2(PSTR("not available"));
#endif
}

// Tracing

int tracing (symbol_t name) {
  int i = 0;
  while (i < TRACEMAX) {
    if (TraceFn[i] == name) return i+1;
    i++;
  }
  return 0;
}

void trace (symbol_t name) {
  if (tracing(name)) error(PSTR("already being traced"), symbol(name));
  int i = 0;
  while (i < TRACEMAX) {
    if (TraceFn[i] == 0) { TraceFn[i] = name; TraceDepth[i] = 0; return; }
    i++;
  }
  error2(PSTR("already tracing " stringify(TRACEMAX) " functions"));
}

void untrace (symbol_t name) {
  int i = 0;
  while (i < TRACEMAX) {
    if (TraceFn[i] == name) { TraceFn[i] = 0; return; }
    i++;
  }
  error(PSTR("not tracing"), symbol(name));
}

// Helper functions

bool consp (object *x) {
  if (x == NULL) return false;
  unsigned int type = x->type;
  return type >= PAIR || type == ZZERO;
}

#define atom(x) (!consp(x))

bool listp (object *x) {
  if (x == NULL) return true;
  unsigned int type = x->type;
  return type >= PAIR || type == ZZERO;
}

#define improperp(x) (!listp(x))

object *quote (object *arg) {
  return cons(bsymbol(QUOTE), cons(arg,NULL));
}

// Radix 40 encoding

builtin_t builtin (symbol_t name) {
  return (builtin_t)(untwist(name) - BUILTINS);
}

symbol_t sym (builtin_t x) {
  return twist(x + BUILTINS);
}

int8_t toradix40 (char ch) {
  if (ch == 0) return 0;
  if (ch >= '0' && ch <= '9') return ch-'0'+1;
  if (ch == '-') return 37; if (ch == '*') return 38; if (ch == '$') return 39;
  ch = ch | 0x20;
  if (ch >= 'a' && ch <= 'z') return ch-'a'+11;
  return -1; // Invalid
}

char fromradix40 (char n) {
  if (n >= 1 && n <= 10) return '0'+n-1;
  if (n >= 11 && n <= 36) return 'a'+n-11;
  if (n == 37) return '-'; if (n == 38) return '*'; if (n == 39) return '$';
  return 0;
}

uint32_t pack40 (char *buffer) {
  int x = 0, j = 0;
  for (int i=0; i<3; i++) {
    x = x * 40 + toradix40(buffer[j]);
    if (buffer[j] != 0) j++;
  }
  return x;
}

bool valid40 (char *buffer) {
  int t = 11;
  for (int i=0; i<3; i++) {
    if (toradix40(buffer[i]) < t) return false;
    if (buffer[i] == 0) break;
    t = 0;
  }
  return true;
}

int8_t digitvalue (char d) {
  if (d>='0' && d<='9') return d-'0';
  d = d | 0x20;
  if (d>='a' && d<='f') return d-'a'+10;
  return 16;
}

int checkinteger (object *obj) {
  if (!integerp(obj)) error(notaninteger, obj);
  return obj->integer;
}

int checkbitvalue (object *obj) {
  if (!integerp(obj)) error(notaninteger, obj);
  int n = obj->integer;
  if (n & ~1) error(PSTR("argument is not a bit value"), obj);
  return n;
}

int checkchar (object *obj) {
  if (!characterp(obj)) error(PSTR("argument is not a character"), obj);
  return obj->chars;
}

object *checkstring (object *obj) {
  if (!stringp(obj)) error(notastring, obj);
  return obj;
}

int isstream (object *obj){
  if (!streamp(obj)) error(PSTR("not a stream"), obj);
  return obj->integer;
}

int isbuiltin (object *obj, builtin_t n) {
  return symbolp(obj) && obj->name == sym(n);
}

bool builtinp (symbol_t name) {
  return (untwist(name) >= BUILTINS);
}

int checkkeyword (object *obj) {
  if (!keywordp(obj)) error(PSTR("argument is not a keyword"), obj);
  builtin_t kname = builtin(obj->name);
  uint8_t context = getminmax(kname);
  if (context != 0 && context != Context) error(invalidkey, obj);
  return ((int)lookupfn(kname));
}

void checkargs (object *args) {
  int nargs = listlength(args);
  checkminmax(Context, nargs);
}

boolean eq (object *arg1, object *arg2) {
  if (arg1 == arg2) return true;  // Same object
  if ((arg1 == nil) || (arg2 == nil)) return false;  // Not both values
  if (arg1->cdr != arg2->cdr) return false;  // Different values
  if (symbolp(arg1) && symbolp(arg2)) return true;  // Same symbol
  if (integerp(arg1) && integerp(arg2)) return true;  // Same integer
  if (characterp(arg1) && characterp(arg2)) return true;  // Same character
  return false;
}

bool equal (object *arg1, object *arg2) {
  if (stringp(arg1) && stringp(arg2)) return (stringcompare(cons(arg1, cons(arg2, nil)), false, false, true) != -1);
  if (consp(arg1) && consp(arg2)) return (equal(car(arg1), car(arg2)) && equal(cdr(arg1), cdr(arg2)));
  return eq(arg1, arg2);
}

int listlength (object *list) {
  int length = 0;
  while (list != NULL) {
    if (improperp(list)) error2(notproper);
    list = cdr(list);
    length++;
  }
  return length;
}

object *checkarguments (object *args, int min, int max) {
  if (args == NULL) error2(noargument);
  args = first(args);
  if (!listp(args)) error(notalist, args);
  int length = listlength(args);
  if (length < min) error(toofewargs, args);
  if (length > max) error(toomanyargs, args);
  return args;
}

// Mathematical helper functions

uint16_t pseudoRandom (int range) {
  if (RandomSeed == 0) RandomSeed++;
  uint16_t l = RandomSeed & 1;
  RandomSeed = RandomSeed >> 1;
  if (l == 1) RandomSeed = RandomSeed ^ 0xD295;
  int dummy; if (RandomSeed == 0) Serial.print((int)&dummy); // Do not remove!
  return RandomSeed % range;
}

object *compare (object *args, bool lt, bool gt, bool eq) {
  int arg1 = checkinteger(first(args));
  args = cdr(args);
  while (args != NULL) {
    int arg2 = checkinteger(first(args));
    if (!lt && (arg1 < arg2)) return nil;
    if (!eq && (arg1 == arg2)) return nil;
    if (!gt && (arg1 > arg2)) return nil;
    arg1 = arg2;
    args = cdr(args);
  }
  return tee;
}

int intpower (int base, int exp) {
  int result = 1;
  while (exp) {
    if (exp & 1) result = result * base;
    exp = exp / 2;
    base = base * base;
  }
  return result;
}

// Association lists

object *testargument (object *args) {
  object *test = bsymbol(EQ);
  if (args != NULL) {
    if (cdr(args) == NULL) error2(PSTR("unpaired keyword"));
    if ((isbuiltin(first(args), TEST))) test = second(args);
    else error(PSTR("unsupported keyword"), first(args));
  }
  return test;
}

object *delassoc (object *key, object **alist) {
  object *list = *alist;
  object *prev = NULL;
  while (list != NULL) {
    object *pair = first(list);
    if (eq(key,car(pair))) {
      if (prev == NULL) *alist = cdr(list);
      else cdr(prev) = cdr(list);
      return key;
    }
    prev = list;
    list = cdr(list);
  }
  return nil;
}

// Array utilities

int nextpower2 (int n) {
  n--; n |= n >> 1; n |= n >> 2; n |= n >> 4;
  n |= n >> 8; n++;
  return n<2 ? 2 : n;
}

object *buildarray (int n, int s, object *def) {
  int s2 = s>>1;
  if (s2 == 1) {
    if (n == 2) return cons(def, def);
    else if (n == 1) return cons(def, NULL);
    else return NULL;
  } else if (n >= s2) return cons(buildarray(s2, s2, def), buildarray(n - s2, s2, def));
  else return cons(buildarray(n, s2, def), nil);
}

object *makearray (object *dims, object *def, bool bitp) {
  int size = 1;
  object *dimensions = dims;
  while (dims != NULL) {
    int d = car(dims)->integer;
    if (d < 0) error2(PSTR("dimension can't be negative"));
    size = size * d;
    dims = cdr(dims);
  }
  // Bit array identified by making first dimension negative
  if (bitp) {
    size = (size + sizeof(int)*8 - 1)/(sizeof(int)*8);
    car(dimensions) = number(-(car(dimensions)->integer));
  }
  object *ptr = myalloc();
  ptr->type = ARRAY;
  object *tree = nil;
  if (size != 0) tree = buildarray(size, nextpower2(size), def);
  ptr->cdr = cons(tree, dimensions);
  return ptr;
}

object **arrayref (object *array, int index, int size) {
  int mask = nextpower2(size)>>1;
  object **p = &car(cdr(array));
  while (mask) {
    if ((index & mask) == 0) p = &(car(*p)); else p = &(cdr(*p));
    mask = mask>>1;
  }
  return p;
}

object **getarray (object *array, object *subs, object *env, int *bit) {
  int index = 0, size = 1, s;
  *bit = -1;
  bool bitp = false;
  object *dims = cddr(array);
  while (dims != NULL && subs != NULL) {
    int d = car(dims)->integer;
    if (d < 0) { d = -d; bitp = true; }
    if (env) s = checkinteger(eval(car(subs), env)); else s = checkinteger(car(subs));
    if (s < 0 || s >= d) error(PSTR("subscript out of range"), car(subs));
    size = size * d;
    index = index * d + s;
    dims = cdr(dims); subs = cdr(subs);
  }
  if (dims != NULL) error2(PSTR("too few subscripts"));
  if (subs != NULL) error2(PSTR("too many subscripts"));
  if (bitp) {
    size = (size + sizeof(int)*8 - 1)/(sizeof(int)*8);
    *bit = index & (sizeof(int)==4 ? 0x1F : 0x0F);
    index = index>>(sizeof(int)==4 ? 5 : 4);
  }
  return arrayref(array, index, size);
}

void rslice (object *array, int size, int slice, object *dims, object *args) {
  int d = first(dims)->integer;
  for (int i = 0; i < d; i++) {
    int index = slice * d + i;
    if (!consp(args)) error2(PSTR("initial contents don't match array type"));
    if (cdr(dims) == NULL) {
      object **p = arrayref(array, index, size);
      *p = car(args);
    } else rslice(array, size, index, cdr(dims), car(args));
    args = cdr(args);
  }
}

object *readarray (int d, object *args) {
  object *list = args;
  object *dims = NULL; object *head = NULL;
  int size = 1;
  for (int i = 0; i < d; i++) {
    if (!listp(list)) error2(PSTR("initial contents don't match array type"));
    int l = listlength(list);
    if (dims == NULL) { dims = cons(number(l), NULL); head = dims; }
    else { cdr(dims) = cons(number(l), NULL); dims = cdr(dims); }
    size = size * l;
    if (list != NULL) list = car(list);
  }
  object *array = makearray(head, NULL, false);
  rslice(array, size, 0, head, args);
  return array;
}

object *readbitarray (gfun_t gfun) {
  char ch = gfun();
  object *head = NULL;
  object *tail = NULL;
  while (!issp(ch) && !isbr(ch)) {
    if (ch != '0' && ch != '1') error2(PSTR("illegal character in bit array"));
    object *cell = cons(number(ch - '0'), NULL);
    if (head == NULL) head = cell;
    else tail->cdr = cell;
    tail = cell;
    ch = gfun();
  }
  LastChar = ch;
  int size = listlength(head);
  object *array = makearray(cons(number(size), NULL), number(0), true);
  size = (size + sizeof(int)*8 - 1)/(sizeof(int)*8);
  int index = 0;
  while (head != NULL) {
    object **loc = arrayref(array, index>>(sizeof(int)==4 ? 5 : 4), size);
    int bit = index & (sizeof(int)==4 ? 0x1F : 0x0F);
    *loc = number((((*loc)->integer) & ~(1<<bit)) | (car(head)->integer)<<bit);
    index++;
    head = cdr(head);
  }
  return array;
}

void pslice (object *array, int size, int slice, object *dims, pfun_t pfun, bool bitp) {
  bool spaces = true;
  if (slice == -1) { spaces = false; slice = 0; }
  int d = first(dims)->integer;
  if (d < 0) d = -d;
  for (int i = 0; i < d; i++) {
    if (i && spaces) pfun(' ');
    int index = slice * d + i;
    if (cdr(dims) == NULL) {
      if (bitp) pint(((*arrayref(array, index>>(sizeof(int)==4 ? 5 : 4), size))->integer)>>
        (index & (sizeof(int)==4 ? 0x1F : 0x0F)) & 1, pfun);
      else printobject(*arrayref(array, index, size), pfun);
    } else { pfun('('); pslice(array, size, index, cdr(dims), pfun, bitp); pfun(')'); }
  }
}

void printarray (object *array, pfun_t pfun) {
  object *dimensions = cddr(array);
  object *dims = dimensions;
  bool bitp = false;
  int size = 1, n = 0;
  while (dims != NULL) {
    int d = car(dims)->integer;
    if (d < 0) { bitp = true; d = -d; }
    size = size * d;
    dims = cdr(dims); n++;
  }
  if (bitp) size = (size + sizeof(int)*8 - 1)/(sizeof(int)*8);
  pfun('#');
  if (n == 1 && bitp) { pfun('*'); pslice(array, size, -1, dimensions, pfun, bitp); }
  else {
    if (n > 1) { pint(n, pfun); pfun('A'); }
    pfun('('); pslice(array, size, 0, dimensions, pfun, bitp); pfun(')');
  }
}

// String utilities

void indent (uint8_t spaces, char ch, pfun_t pfun) {
  for (uint8_t i=0; i<spaces; i++) pfun(ch);
}

object *startstring () {
  object *string = newstring();
  GlobalString = string;
  GlobalStringTail = string;
  return string;
}

object *princtostring (object *arg) {
  object *obj = startstring();
  prin1object(arg, pstr);
  return obj;
}

void buildstring (char ch, object **tail) {
  object *cell;
  if (cdr(*tail) == NULL) {
    cell = myalloc(); cdr(*tail) = cell;
  } else if (((*tail)->chars & 0xFF) == 0) {
    (*tail)->chars |= ch; return;
  } else {
    cell = myalloc(); car(*tail) = cell;
  }
  car(cell) = NULL; cell->chars = ch<<8; *tail = cell;
}

object *copystring (object *arg) {
  object *obj = newstring();
  object *ptr = obj;
  arg = cdr(arg);
  while (arg != NULL) {
    object *cell =  myalloc(); car(cell) = NULL;
    if (cdr(obj) == NULL) cdr(obj) = cell; else car(ptr) = cell;
    ptr = cell;
    ptr->chars = arg->chars;
    arg = car(arg);
  }
  return obj;
}

object *readstring (uint8_t delim, bool esc, gfun_t gfun) {
  object *obj = newstring();
  object *tail = obj;
  int ch = gfun();
  if (ch == -1) return nil;
  while ((ch != delim) && (ch != -1)) {
    if (esc && ch == '\\') ch = gfun();
    buildstring(ch, &tail);
    ch = gfun();
  }
  return obj;
}

int stringlength (object *form) {
  int length = 0;
  form = cdr(form);
  while (form != NULL) {
    int chars = form->chars;
    for (int i=(sizeof(int)-1)*8; i>=0; i=i-8) {
      if (chars>>i & 0xFF) length++;
    }
    form = car(form);
  }
  return length;
}

object **getcharplace (object *string, int n, int *shift) {
  object **arg = &cdr(string);
  int top;
  if (sizeof(int) == 4) { top = n>>2; *shift = 3 - (n&3); }
  else { top = n>>1; *shift = 1 - (n&1); }
  *shift = - (*shift + 2);
  for (int i=0; i<top; i++) {
    if (*arg == NULL) break;
    arg = &car(*arg);
  }
  return arg;
}

uint8_t nthchar (object *string, int n) {
  int shift;
  object **arg = getcharplace(string, n, &shift);
  if (*arg == NULL) return 0;
  return (((*arg)->chars)>>((-shift-2)<<3)) & 0xFF;
}

int gstr () {
  if (LastChar) {
    char temp = LastChar;
    LastChar = 0;
    return temp;
  }
  char c = nthchar(GlobalString, GlobalStringIndex++);
  if (c != 0) return c;
  return '\n'; // -1?
}

void pstr (char c) {
  buildstring(c, &GlobalStringTail);
}

object *lispstring (char *s) {
  object *obj = newstring();
  object *tail = obj;
  while(1) {
    char ch = *s++;
    if (ch == 0) break;
    if (ch == '\\') ch = *s++;
    buildstring(ch, &tail);
  }
  return obj;
}

int stringcompare (object *args, bool lt, bool gt, bool eq) {
  object *arg1 = checkstring(first(args));
  object *arg2 = checkstring(second(args));
  arg1 = cdr(arg1); arg2 = cdr(arg2);
  int m = 0; chars_t a = 0, b = 0;
  while ((arg1 != NULL) || (arg2 != NULL)) {
    if (arg1 == NULL) return lt ? m : -1;
    if (arg2 == NULL) return gt ? m : -1;
    a = arg1->chars; b = arg2->chars;
    if ((a < b && lt) || ((a > b) && gt)) {
      m = m + sizeof(int);
      while (a != b) { m--; a = a >> 8; b = b >> 8; }
      return m;
    } else if ((a < b && !lt) || ((a > b) && !gt)) return -1;
    // if (a < b) { if (lt) {  while (a != b) { m--; a = a >> 8; b = b >> 8; } return m; } else return -1; }
    // if (a > b) { if (gt) { m = m + sizeof(int); while (a != b) { m--; a = a >> 8; b = b >> 8; } return m; } else return -1; }
    arg1 = car(arg1); arg2 = car(arg2);
    m = m + sizeof(int);
  }
  if (eq) { m = m - sizeof(int); while (a != 0) { m++; a = a << 8;} return m;} else return -1;
}

object *documentation (object *arg, object *env) {
  if (arg == NULL) return nil;
  if (!symbolp(arg)) error(notasymbol, arg);
  object *pair = findpair(arg, env);
  if (pair != NULL) {
    object *val = cdr(pair);
    if (listp(val) && first(val)->name == sym(LAMBDA) && cdr(val) != NULL && cddr(val) != NULL) {
      if (stringp(third(val))) return third(val);
    }
  }
  symbol_t docname = arg->name;
  if (!builtinp(docname)) return nil;
  char *docstring = lookupdoc(builtin(docname));
  if (docstring == NULL) return nil;
  object *obj = startstring();
  pfstring(docstring, pstr);
  return obj;
}

object *apropos (object *arg, bool print) {
  char buf[17], buf2[33];
  char *part = cstring(princtostring(arg), buf, 17);
  object *result = cons(NULL, NULL);
  object *ptr = result;
  // User-defined?
  object *globals = GlobalEnv;
  while (globals != NULL) {
    object *pair = first(globals);
    object *var = car(pair);
    object *val = cdr(pair);
    char *full = cstring(princtostring(var), buf2, 33);
    if (strstr(full, part) != NULL) {
      if (print) {
        printsymbol(var, pserial); pserial(' '); pserial('(');
        if (consp(val) && symbolp(car(val)) && builtin(car(val)->name) == LAMBDA) pfstring(PSTR("user function"), pserial);
        else if (consp(val) && car(val)->type == CODE) pfstring(PSTR("code"), pserial);
        else pfstring(PSTR("user symbol"), pserial);
        pserial(')'); pln(pserial);
      } else {
        cdr(ptr) = cons(var, NULL); ptr = cdr(ptr);
      }
    }
    globals = cdr(globals);
    testescape();
  }
  // Built-in?
  int entries = tablesize(0) + tablesize(1);
  for (int i = 0; i < entries; i++) {
    if (findsubstring(part, (builtin_t)i)) {
      if (print) {
        uint8_t fntype = getminmax(i)>>6;
        pbuiltin((builtin_t)i, pserial); pserial(' '); pserial('(');
        if (fntype == FUNCTIONS) pfstring(PSTR("function"), pserial);
        else if (fntype == SPECIAL_FORMS) pfstring(PSTR("special form"), pserial);
        else pfstring(PSTR("symbol/keyword"), pserial);
        pserial(')'); pln(pserial);
      } else {
        cdr(ptr) = cons(bsymbol(i), NULL); ptr = cdr(ptr);
      }
    }
    testescape();
  }
  return cdr(result);
}

char *cstring (object *form, char *buffer, int buflen) {
  form = cdr(checkstring(form));
  int index = 0;
  while (form != NULL) {
    int chars = form->integer;
    for (int i=(sizeof(int)-1)*8; i>=0; i=i-8) {
      char ch = chars>>i & 0xFF;
      if (ch) {
        if (index >= buflen-1) error2(PSTR("no room for string"));
        buffer[index++] = ch;
      }
    }
    form = car(form);
  }
  buffer[index] = '\0';
  return buffer;
}

// Lookup variable in environment

object *value (symbol_t n, object *env) {
  while (env != NULL) {
    object *pair = car(env);
    if (pair != NULL && car(pair)->name == n) return pair;
    env = cdr(env);
  }
  return nil;
}

object *findpair (object *var, object *env) {
  symbol_t name = var->name;
  object *pair = value(name, env);
  if (pair == NULL) pair = value(name, GlobalEnv);
  return pair;
}

bool boundp (object *var, object *env) {
  if (!symbolp(var)) error(notasymbol, var);
  return (findpair(var, env) != NULL);
}

object *findvalue (object *var, object *env) {
  object *pair = findpair(var, env);
  if (pair == NULL) error(PSTR("unknown variable"), var);
  return pair;
}

// Handling closures

object *closure (int tc, symbol_t name, object *function, object *args, object **env) {
  object *state = car(function);
  function = cdr(function);
  int trace = 0;
  if (name) trace = tracing(name);
  if (trace) {
    indent(TraceDepth[trace-1]<<1, ' ', pserial);
    pint(TraceDepth[trace-1]++, pserial);
    pserial(':'); pserial(' '); pserial('('); printsymbol(symbol(name), pserial);
  }
  object *params = first(function);
  if (!listp(params)) errorsym(name, notalist, params);
  function = cdr(function);
  // Dropframe
  if (tc) {
    if (*env != NULL && car(*env) == NULL) {
      pop(*env);
      while (*env != NULL && car(*env) != NULL) pop(*env);
    } else push(nil, *env);
  }
  // Push state
  while (consp(state)) {
    object *pair = first(state);
    push(pair, *env);
    state = cdr(state);
  }
  // Add arguments to environment
  bool optional = false;
  while (params != NULL) {
    object *value;
    object *var = first(params);
    if (isbuiltin(var, OPTIONAL)) optional = true;
    else {
      if (consp(var)) {
        if (!optional) errorsym(name, PSTR("invalid default value"), var);
        if (args == NULL) value = eval(second(var), *env);
        else { value = first(args); args = cdr(args); }
        var = first(var);
        if (!symbolp(var)) errorsym(name, PSTR("illegal optional parameter"), var);
      } else if (!symbolp(var)) {
        errorsym(name, PSTR("illegal function parameter"), var);
      } else if (isbuiltin(var, AMPREST)) {
        params = cdr(params);
        var = first(params);
        value = args;
        args = NULL;
      } else {
        if (args == NULL) {
          if (optional) value = nil;
          else errorsym2(name, toofewargs);
        } else { value = first(args); args = cdr(args); }
      }
      push(cons(var,value), *env);
      if (trace) { pserial(' '); printobject(value, pserial); }
    }
    params = cdr(params);
  }
  if (args != NULL) errorsym2(name, toomanyargs);
  if (trace) { pserial(')'); pln(pserial); }
  // Do an implicit progn
  if (tc) push(nil, *env);
  return tf_progn(function, *env);
}

object *apply (object *function, object *args, object *env) {
  if (symbolp(function)) {
    builtin_t fname = builtin(function->name);
    if ((fname < ENDFUNCTIONS) && ((getminmax(fname)>>6) == FUNCTIONS)) {
      Context = fname;
      checkargs(args);
      return ((fn_ptr_type)lookupfn(fname))(args, env);
    } else function = eval(function, env);
  }
  if (consp(function) && isbuiltin(car(function), LAMBDA)) {
    object *result = closure(0, sym(NIL), function, args, &env);
    return eval(result, env);
  }
  if (consp(function) && isbuiltin(car(function), CLOSURE)) {
    function = cdr(function);
    object *result = closure(0, sym(NIL), function, args, &env);
    return eval(result, env);
  }
  error(PSTR("illegal function"), function);
  return NULL;
}

// In-place operations

object **place (object *args, object *env, int *bit) {
  *bit = -1;
  if (atom(args)) return &cdr(findvalue(args, env));
  object* function = first(args);
  if (symbolp(function)) {
    symbol_t sname = function->name;
    if (sname == sym(CAR) || sname == sym(FIRST)) {
      object *value = eval(second(args), env);
      if (!listp(value)) error(canttakecar, value);
      return &car(value);
    }
    if (sname == sym(CDR) || sname == sym(REST)) {
      object *value = eval(second(args), env);
      if (!listp(value)) error(canttakecdr, value);
      return &cdr(value);
    }
    if (sname == sym(NTH)) {
      int index = checkinteger(eval(second(args), env));
      object *list = eval(third(args), env);
      if (atom(list)) { Context = NTH; error(PSTR("second argument is not a list"), list); }
      int i = index;
      while (i > 0) {
        list = cdr(list);
        if (list == NULL) { Context = NTH; error(indexrange, number(index)); }
        i--;
      }
      return &car(list);
    }
    if (sname == sym(CHAR)) {
      int index = checkinteger(eval(third(args), env));
      object *string = checkstring(eval(second(args), env));
      object **loc = getcharplace(string, index, bit);
      if ((*loc) == NULL || (((((*loc)->chars)>>((-(*bit)-2)<<3)) & 0xFF) == 0)) { Context = CHAR; error(indexrange, number(index)); }
      return loc;
    }
    if (sname == sym(AREF)) {
      object *array = eval(second(args), env);
      if (!arrayp(array)) { Context = AREF; error(PSTR("first argument is not an array"), array); }
      return getarray(array, cddr(args), env, bit);
    }
  }
  error2(PSTR("illegal place"));
  return nil;
}

object *incfdecf (object *args, int increment, object *env) {
  int bit;
  object **loc = place(first(args), env, &bit);
  if (bit < -1) error2(notanumber);
  int result = checkinteger(*loc);
  args = cdr(args);
  object *inc = (args != NULL) ? eval(first(args), env) : NULL;

  if (bit != -1) {
    if (inc != NULL) increment = checkbitvalue(inc);
    int newvalue = (((*loc)->integer)>>bit & 1) + increment;

    if (newvalue & ~1) error2(PSTR("result is not a bit value"));
    *loc = number((((*loc)->integer) & ~(1<<bit)) | newvalue<<bit);
    return number(newvalue);
  }

  if (inc != NULL) increment = increment * checkinteger(inc);
  #if defined(checkoverflow)
  if (increment < 1) { if (INT_MIN - increment > result) error2(overflow); }
  else { if (INT_MAX - increment < result) error2(overflow); }
  #endif
  result = result + increment;
  *loc = number(result);
  return *loc;
}

// Checked car and cdr

object *carx (object *arg) {
  if (!listp(arg)) error(canttakecar, arg);
  if (arg == nil) return nil;
  return car(arg);
}

object *cdrx (object *arg) {
  if (!listp(arg)) error(canttakecdr, arg);
  if (arg == nil) return nil;
  return cdr(arg);
}

object *cxxxr (object *args, uint8_t pattern) {
  object *arg = first(args);
  while (pattern != 1) {
    if ((pattern & 1) == 0) arg = carx(arg); else arg = cdrx(arg);
    pattern = pattern>>1;
  }
  return arg;
}

// Mapping helper functions

object *mapcl (object *args, object *env, bool mapl) {
  object *function = first(args);
  args = cdr(args);
  object *result = first(args);
  protect(result);
  object *params = cons(NULL, NULL);
  protect(params);
  // Make parameters
  while (true) {
    object *tailp = params;
    object *lists = args;
    while (lists != NULL) {
      object *list = car(lists);
      if (list == NULL) {
         unprotect(); unprotect();
         return result;
      }
      if (improperp(list)) error(notproper, list);
      object *item = mapl ? list : first(list);
      object *obj = cons(item, NULL);
      car(lists) = cdr(list);
      cdr(tailp) = obj; tailp = obj;
      lists = cdr(lists);
    }
    apply(function, cdr(params), env);
  }
}

void mapcarfun (object *result, object **tail) {
  object *obj = cons(result,NULL);
  cdr(*tail) = obj; *tail = obj;
}

void mapcanfun (object *result, object **tail) {
  if (cdr(*tail) != NULL) error(notproper, *tail);
  while (consp(result)) {
    cdr(*tail) = result; *tail = result;
    result = cdr(result);
  }
}

object *mapcarcan (object *args, object *env, mapfun_t fun, bool maplist) {
  object *function = first(args);
  args = cdr(args);
  object *params = cons(NULL, NULL);
  protect(params);
  object *head = cons(NULL, NULL);
  protect(head);
  object *tail = head;
  // Make parameters
  while (true) {
    object *tailp = params;
    object *lists = args;
    while (lists != NULL) {
      object *list = car(lists);
      if (list == NULL) {
         unprotect(); unprotect();
         return cdr(head);
      }
      if (improperp(list)) error(notproper, list);
      object *item = maplist ? list : first(list);
      object *obj = cons(item, NULL);
      car(lists) = cdr(list);
      cdr(tailp) = obj; tailp = obj;
      lists = cdr(lists);
    }
    object *result = apply(function, cdr(params), env);
    fun(result, &tail);
  }
}

object *dobody (object *args, object *env, bool star) {
  object *varlist = first(args), *endlist = second(args);
  object *head = cons(NULL, NULL);
  protect(head);
  object *ptr = head;
  object *newenv = env;
  while (varlist != NULL) {
    object *varform = first(varlist);
    object *var, *init = NULL, *step = NULL;
    if (atom(varform)) var = varform;
    else {
      var = first(varform);
      varform = cdr(varform);
      if (varform != NULL) {
        init = eval(first(varform), env);
        varform = cdr(varform);
        if (varform != NULL) step = cons(first(varform), NULL);
      }
    }
    object *pair = cons(var, init);
    push(pair, newenv);
    if (star) env = newenv;
    object *cell = cons(cons(step, pair), NULL);
    cdr(ptr) = cell; ptr = cdr(ptr);
    varlist = cdr(varlist);
  }
  env = newenv;
  head = cdr(head);
  object *endtest = first(endlist), *results = cdr(endlist);
  while (eval(endtest, env) == NULL) {
    object *forms = cddr(args);
    while (forms != NULL) {
    object *result = eval(car(forms), env);
      if (tstflag(RETURNFLAG)) {
        clrflag(RETURNFLAG);
        return result;
      }
      forms = cdr(forms);
    }
    object *varlist = head;
    int count = 0;
    while (varlist != NULL) {
      object *varform = first(varlist);
      object *step = car(varform), *pair = cdr(varform);
      if (step != NULL) {
        object *val = eval(first(step), env);
        if (star) {
          cdr(pair) = val;
        } else {
          push(val, GCStack);
          push(pair, GCStack);
          count++;
        }
      }
      varlist = cdr(varlist);
    }
    while (count > 0) {
      cdr(car(GCStack)) = car(cdr(GCStack));
      pop(GCStack); pop(GCStack);
      count--;
    }
  }
  unprotect();
  return eval(tf_progn(results, env), env);
}

// I2C interface for AVR platforms, uses much less RAM than Arduino Wire

uint32_t const FREQUENCY = 400000L;  // Hardware I2C clock in Hz
uint32_t const T_RISE = 300L;        // Rise time

void I2Cinit (bool enablePullup) {
(void) enablePullup;
uint32_t baud = ((F_CPU/FREQUENCY) - (((F_CPU*T_RISE)/1000)/1000)/1000 - 10)/2;
TWI0.MBAUD = (uint8_t)baud;
TWI0.MCTRLA = TWI_ENABLE_bm;                                    // Enable as master, no interrupts
TWI0.MSTATUS = TWI_BUSSTATE_IDLE_gc;
}

int I2Cread () {
if (I2Ccount != 0) I2Ccount--;
while (!(TWI0.MSTATUS & TWI_RIF_bm));                           // Wait for read interrupt flag
uint8_t data = TWI0.MDATA;
// Check slave sent ACK?
if (I2Ccount != 0) TWI0.MCTRLB = TWI_MCMD_RECVTRANS_gc;         // ACK = more bytes to read
else TWI0.MCTRLB = TWI_ACKACT_NACK_gc;                          // Send NAK
return data;
}

bool I2Cwrite (uint8_t data) {
TWI0.MCTRLB = TWI_MCMD_RECVTRANS_gc;                            // Prime transaction
TWI0.MDATA = data;                                              // Send data
while (!(TWI0.MSTATUS & TWI_WIF_bm));                           // Wait for write to complete

if (TWI0.MSTATUS & (TWI_ARBLOST_bm | TWI_BUSERR_bm)) return false; // Fails if bus error or arblost
return !(TWI0.MSTATUS & TWI_RXACK_bm);                          // Returns true if slave gave an ACK
}

bool I2Cstart (uint8_t address, uint8_t read) {
TWI0.MADDR = address<<1 | read;                                 // Send START condition
while (!(TWI0.MSTATUS & (TWI_WIF_bm | TWI_RIF_bm)));            // Wait for write or read interrupt flag
if (TWI0.MSTATUS & TWI_ARBLOST_bm) {                            // Arbitration lost or bus error
  while (!((TWI0.MSTATUS & TWI_BUSSTATE_gm) == TWI_BUSSTATE_IDLE_gc)); // Wait for bus to return to idle state
  return false;
} else if (TWI0.MSTATUS & TWI_RXACK_bm) {                       // Address not acknowledged by client
  TWI0.MCTRLB |= TWI_MCMD_STOP_gc;                              // Send stop condition
  while (!((TWI0.MSTATUS & TWI_BUSSTATE_gm) == TWI_BUSSTATE_IDLE_gc)); // Wait for bus to return to idle state
  return false;
}
return true;                                                    // Return true if slave gave an ACK
}

bool I2Crestart (uint8_t address, uint8_t read) {
  return I2Cstart(address, read);
}

void I2Cstop (uint8_t read) {
(void) read;
TWI0.MCTRLB |= TWI_MCMD_STOP_gc;                                // Send STOP
while (!((TWI0.MSTATUS & TWI_BUSSTATE_gm) == TWI_BUSSTATE_IDLE_gc)); // Wait for bus to return to idle state
}

// Streams

inline int spiread () { return SPI.transfer(0); }
inline int serial1read () { while (!Serial1.available()) testescape(); return Serial1.read(); }

#if defined(sdcardsupport)
File SDpfile, SDgfile;
inline int SDread () {
  if (LastChar) {
    char temp = LastChar;
    LastChar = 0;
    return temp;
  }
  return SDgfile.read();
}
#endif

void serialbegin (int address, int baud) {
  if (address == 1) Serial1.begin((long)baud*100);
  else error(PSTR("port not supported"), number(address));
}

void serialend (int address) {
  if (address == 1) {Serial1.flush(); Serial1.end(); }
}

gfun_t gstreamfun (object *args) {
  int streamtype = SERIALSTREAM;
  int address = 0;
  gfun_t gfun = gserial;
  if (args != NULL) {
    int stream = isstream(first(args));
    streamtype = stream>>8; address = stream & 0xFF;
  }
  if (streamtype == I2CSTREAM) gfun = (gfun_t)I2Cread;
  else if (streamtype == SPISTREAM) gfun = spiread;
  else if (streamtype == SERIALSTREAM) {
    if (address == 0) gfun = gserial;
    #if defined(CPU_ATmega1284P) || defined(CPU_AVR128DX48)
    else if (address == 1) gfun = serial1read;
    #elif defined(CPU_ATmega2560)
    else if (address == 1) gfun = serial1read;
    else if (address == 2) gfun = serial2read;
    else if (address == 3) gfun = serial3read;
    #endif
  }
  #if defined(sdcardsupport)
  else if (streamtype == SDSTREAM) gfun = (gfun_t)SDread;
  #endif
  else error2(unknownstreamtype);
  return gfun;
}

inline void spiwrite (char c) { SPI.transfer(c); }
inline void serial1write (char c) { Serial1.write(c); }

#if defined(sdcardsupport)
inline void SDwrite (char c) { int w = SDpfile.write(c); if (w != 1) { Context = NIL; error2(PSTR("failed to write to file")); } }
#endif

pfun_t pstreamfun (object *args) {
  int streamtype = SERIALSTREAM;
  int address = 0;
  pfun_t pfun = pserial;
  if (args != NULL && first(args) != NULL) {
    int stream = isstream(first(args));
    streamtype = stream>>8; address = stream & 0xFF;
  }
  if (streamtype == I2CSTREAM) pfun = (pfun_t)I2Cwrite;
  else if (streamtype == SPISTREAM) pfun = spiwrite;
  else if (streamtype == SERIALSTREAM) {
    if (address == 0) pfun = pserial;
    else if (address == 1) pfun = serial1write;
  }
  else if (streamtype == STRINGSTREAM) {
    pfun = pstr;
  }
  #if defined(sdcardsupport)
  else if (streamtype == SDSTREAM) pfun = (pfun_t)SDwrite;
  #endif
  else error2(unknownstreamtype);
  return pfun;
}

// Check pins

void checkanalogread (int pin) {
  if (!(pin>=0 && pin<=7)) error(invalidpin, number(pin));
}

void checkanalogwrite (int pin) {
  if (!(pin==3 || pin==4 || pin==6 || pin==7 || (pin>=12 && pin<=15))) error(invalidpin, number(pin));
}

// Note

const int scale[] PROGMEM = {4186,4435,4699,4978,5274,5588,5920,6272,6645,7040,7459,7902};

void playnote (int pin, int note, int octave) {
  int oct = octave + note/12;
  int prescaler = 8 - oct;
  if (prescaler<0 || prescaler>8) error(PSTR("octave out of range"), number(oct));
  tone(pin, pgm_read_word(&scale[note%12])>>prescaler);
}

void nonote (int pin) {
  noTone(pin);
}

// Sleep

void initsleep () {
  set_sleep_mode(SLEEP_MODE_PWR_DOWN);
}

void sleep () {
  delay(100);  // Give serial time to settle
  sleep_enable();
  sleep_cpu();
}

void doze (int secs) {
  delay(1000*secs);
}

// Prettyprint

const int PPINDENT = 2;
const int PPWIDTH = 42;

void pcount (char c) {
  if (c == '\n') PrintCount++;
  PrintCount++;
}

uint8_t atomwidth (object *obj) {
  PrintCount = 0;
  printobject(obj, pcount);
  return PrintCount;
}

uint8_t basewidth (object *obj, uint8_t base) {
  PrintCount = 0;
  pintbase(obj->integer, base, pcount);
  return PrintCount;
}

bool quoted (object *obj) {
  return (consp(obj) && car(obj) != NULL && car(obj)->name == sym(QUOTE) && consp(cdr(obj)) && cddr(obj) == NULL);
}

int subwidth (object *obj, int w) {
  if (atom(obj)) return w - atomwidth(obj);
  if (quoted(obj)) obj = car(cdr(obj));
  return subwidthlist(obj, w - 1);
}

int subwidthlist (object *form, int w) {
  while (form != NULL && w >= 0) {
    if (atom(form)) return w - (2 + atomwidth(form));
    w = subwidth(car(form), w - 1);
    form = cdr(form);
  }
  return w;
}

void superprint (object *form, int lm, pfun_t pfun) {
  if (atom(form)) {
    if (symbolp(form) && form->name == sym(NOTHING)) printsymbol(form, pfun);
    else printobject(form, pfun);
  } else if (quoted(form)) {
    pfun('\'');
    superprint(car(cdr(form)), lm + 1, pfun);
  } else {
    lm = lm + PPINDENT;
    bool fits = (subwidth(form, PPWIDTH - lm - PPINDENT) >= 0);
    int special = 0, extra = 0; bool separate = true;
    object *arg = car(form);
    if (symbolp(arg) && builtinp(arg->name)) {
      uint8_t minmax = getminmax(builtin(arg->name));
      if (minmax == 0327 || minmax == 0313) special = 2; // defun, setq, setf, defvar
      else if (minmax == 0317 || minmax == 0017 || minmax == 0117 || minmax == 0123) special = 1;
    }
    while (form != NULL) {
      if (atom(form)) { pfstring(PSTR(" . "), pfun); printobject(form, pfun); pfun(')'); return; }
      else if (separate) {
        pfun('(');
        separate = false;
      } else if (special) {
        pfun(' ');
        special--;
      } else if (fits) {
        pfun(' ');
      } else { pln(pfun); indent(lm, ' ', pfun); }
      superprint(car(form), lm+extra, pfun);
      form = cdr(form);
    }
    pfun(')');
  }
}

object *edit (object *fun) {
  while (1) {
    if (tstflag(EXITEDITOR)) return fun;
    char c = gserial();
    if (c == 'q') setflag(EXITEDITOR);
    else if (c == 'b') return fun;
    else if (c == 'r') fun = read(gserial);
    else if (c == '\n') { pfl(pserial); superprint(fun, 0, pserial); pln(pserial); }
    else if (c == 'c') fun = cons(read(gserial), fun);
    else if (atom(fun)) pserial('!');
    else if (c == 'd') fun = cons(car(fun), edit(cdr(fun)));
    else if (c == 'a') fun = cons(edit(car(fun)), cdr(fun));
    else if (c == 'x') fun = cdr(fun);
    else pserial('?');
  }
}

// Assembler

#if defined(CPU_ATmega1284P)
#define CODE_ADDRESS 0x1bb00
#elif defined(CPU_AVR128DX48)
#define CODE_ADDRESS 0x1be00
#endif

#if defined(CODESIZE)
object *call (int entry, int nargs, object *args, object *env) {
  (void) env;
  int param[4];
  for (int i=0; i<nargs; i++) {
    object *arg = first(args);
    if (integerp(arg)) param[i] = arg->integer;
    else param[i] = (uintptr_t)arg;
    args = cdr(args);
  }
  uint32_t address = (CODE_ADDRESS + entry)>>1; // Code addresses are word addresses on AVR
  int w = ((intfn_ptr_type)address)(param[0], param[1], param[2], param[3]);
  return number(w);
}

void putcode (object *arg, int origin, int pc) {
  int code = checkinteger(arg);
  uint8_t hi = (code>>8) & 0xff;
  uint8_t lo = code & 0xff;
  MyCode[origin+pc] = lo;            // Little-endian
  MyCode[origin+pc+1] = hi;
  #if defined(assemblerlist)
  printhex2(pc>>8, pserial); printhex2(pc, pserial); pserial(' ');
  printhex2(lo, pserial); pserial(' '); printhex2(hi, pserial); pserial(' ');
  #endif
}

int assemble (int pass, int origin, object *entries, object *env, object *pcpair) {
  int pc = 0; cdr(pcpair) = number(pc);
  while (entries != NULL) {
    object *arg = first(entries);
    if (symbolp(arg)) {
      if (pass == 2) {
        #if defined(assemblerlist)
        printhex2(pc>>8, pserial); printhex2(pc, pserial);
        indent(7, ' ', pserial);
        printobject(arg, pserial); pln(pserial);
        #endif
      } else {
        object *pair = findvalue(arg, env);
        cdr(pair) = number(pc);
      }
    } else {
      object *argval = eval(arg, env);
      if (listp(argval)) {
        object *arglist = argval;
        while (arglist != NULL) {
          if (pass == 2) {
            putcode(first(arglist), origin, pc);
            #if defined(assemblerlist)
            if (arglist == argval) superprint(arg, 0, pserial);
            pln(pserial);
            #endif
          }
          pc = pc + 2;
          cdr(pcpair) = number(pc);
          arglist = cdr(arglist);
        }
      } else if (integerp(argval)) {
        if (pass == 2) {
          putcode(argval, origin, pc);
          #if defined(assemblerlist)
          superprint(arg, 0, pserial); pln(pserial);
          #endif
        }
        pc = pc + 2;
        cdr(pcpair) = number(pc);
      } else error(PSTR("illegal entry"), arg);
    }
    entries = cdr(entries);
  }
  // Round up to multiple of 2 to give code size
  if (pc%2 != 0) pc = pc + 2 - pc%2;
  return pc;
}
#endif

// Special forms

object *sp_quote (object *args, object *env) {
  (void) env;
  return first(args);
}

object *sp_or (object *args, object *env) {
  while (args != NULL) {
    object *val = eval(car(args), env);
    if (val != NULL) return val;
    args = cdr(args);
  }
  return nil;
}

object *sp_defun (object *args, object *env) {
  (void) env;
  object *var = first(args);
  if (!symbolp(var)) error(notasymbol, var);
  object *val = cons(bsymbol(LAMBDA), cdr(args));
  object *pair = value(var->name, GlobalEnv);
  if (pair != NULL) cdr(pair) = val;
  else push(cons(var, val), GlobalEnv);
  return var;
}

object *sp_defvar (object *args, object *env) {
  object *var = first(args);
  if (!symbolp(var)) error(notasymbol, var);
  object *val = NULL;
  args = cdr(args);
  if (args != NULL) { setflag(NOESC); val = eval(first(args), env); clrflag(NOESC); }
  object *pair = value(var->name, GlobalEnv);
  if (pair != NULL) cdr(pair) = val;
  else push(cons(var, val), GlobalEnv);
  return var;
}

object *sp_setq (object *args, object *env) {
  object *arg = nil;
  while (args != NULL) {
    if (cdr(args) == NULL) error2(oddargs);
    object *pair = findvalue(first(args), env);
    arg = eval(second(args), env);
    cdr(pair) = arg;
    args = cddr(args);
  }
  return arg;
}

object *sp_loop (object *args, object *env) {
  object *start = args;
  for (;;) {
    args = start;
    while (args != NULL) {
      object *result = eval(car(args),env);
      if (tstflag(RETURNFLAG)) {
        clrflag(RETURNFLAG);
        return result;
      }
      args = cdr(args);
    }
    testescape();
  }
}

object *sp_return (object *args, object *env) {
  object *result = eval(tf_progn(args,env), env);
  setflag(RETURNFLAG);
  return result;
}

object *sp_push (object *args, object *env) {
  int bit;
  object *item = eval(first(args), env);
  object **loc = place(second(args), env, &bit);
  if (bit != -1) error2(invalidarg);
  push(item, *loc);
  return *loc;
}

object *sp_pop (object *args, object *env) {
  int bit;
  object *arg = first(args);
  if (arg == NULL) error2(invalidarg);
  object **loc = place(arg, env, &bit);
  if (bit < -1) error(invalidarg, arg);
  if (!consp(*loc)) error(notalist, *loc);
  object *result = car(*loc);
  pop(*loc);
  return result;
}

// Accessors

object *sp_incf (object *args, object *env) {
  return incfdecf(args, 1, env);
}

object *sp_decf (object *args, object *env) {
  return incfdecf(args, -1, env);
}

object *sp_setf (object *args, object *env) {
  int bit;
  object *arg = nil;
  while (args != NULL) {
    if (cdr(args) == NULL) error2(oddargs);
    object **loc = place(first(args), env, &bit);
    arg = eval(second(args), env);
    if (bit == -1) *loc = arg;
    else if (bit < -1) (*loc)->chars = ((*loc)->chars & ~(0xff<<((-bit-2)<<3))) | checkchar(arg)<<((-bit-2)<<3);
    else *loc = number((checkinteger(*loc) & ~(1<<bit)) | checkbitvalue(arg)<<bit);
    args = cddr(args);
  }
  return arg;
}

// Other special forms

object *sp_dolist (object *args, object *env) {
  object *params = checkarguments(args, 2, 3);
  object *var = first(params);
  object *list = eval(second(params), env);
  protect(list); // Don't GC the list
  object *pair = cons(var,nil);
  push(pair,env);
  params = cddr(params);
  args = cdr(args);
  while (list != NULL) {
    if (improperp(list)) error(notproper, list);
    cdr(pair) = first(list);
    object *forms = args;
    while (forms != NULL) {
      object *result = eval(car(forms), env);
      if (tstflag(RETURNFLAG)) {
        clrflag(RETURNFLAG);
        unprotect();
        return result;
      }
      forms = cdr(forms);
    }
    list = cdr(list);
  }
  cdr(pair) = nil;
  unprotect();
  if (params == NULL) return nil;
  return eval(car(params), env);
}

object *sp_dotimes (object *args, object *env) {
  object *params = checkarguments(args, 2, 3);
  object *var = first(params);
  int count = checkinteger(eval(second(params), env));
  int index = 0;
  params = cddr(params);
  object *pair = cons(var,number(0));
  push(pair,env);
  args = cdr(args);
  while (index < count) {
    cdr(pair) = number(index);
    object *forms = args;
    while (forms != NULL) {
      object *result = eval(car(forms), env);
      if (tstflag(RETURNFLAG)) {
        clrflag(RETURNFLAG);
        return result;
      }
      forms = cdr(forms);
    }
    index++;
  }
  cdr(pair) = number(index);
  if (params == NULL) return nil;
  return eval(car(params), env);
}

object *sp_do (object *args, object *env) {
  return dobody(args, env, false);
}

object *sp_dostar (object *args, object *env) {
  return dobody(args, env, true);
}

object *sp_trace (object *args, object *env) {
  (void) env;
  while (args != NULL) {
    object *var = first(args);
    if (!symbolp(var)) error(notasymbol, var);
    trace(var->name);
    args = cdr(args);
  }
  int i = 0;
  while (i < TRACEMAX) {
    if (TraceFn[i] != 0) args = cons(symbol(TraceFn[i]), args);
    i++;
  }
  return args;
}

object *sp_untrace (object *args, object *env) {
  (void) env;
  if (args == NULL) {
    int i = 0;
    while (i < TRACEMAX) {
      if (TraceFn[i] != 0) args = cons(symbol(TraceFn[i]), args);
      TraceFn[i] = 0;
      i++;
    }
  } else {
    while (args != NULL) {
      object *var = first(args);
      if (!symbolp(var)) error(notasymbol, var);
      untrace(var->name);
      args = cdr(args);
    }
  }
  return args;
}

object *sp_formillis (object *args, object *env) {
  object *param = checkarguments(args, 0, 1);
  unsigned long start = millis();
  unsigned long now, total = 0;
  if (param != NULL) total = checkinteger(eval(first(param), env));
  eval(tf_progn(cdr(args),env), env);
  do {
    now = millis() - start;
    testescape();
  } while (now < total);
  if (now <= INT_MAX) return number(now);
  return nil;
}

object *sp_time (object *args, object *env) {
  unsigned long start = millis();
  object *result = eval(first(args), env);
  unsigned long elapsed = millis() - start;
  printobject(result, pserial);
  pfstring(PSTR("\nTime: "), pserial);
  if (elapsed < 1000) {
    pint(elapsed, pserial);
    pfstring(PSTR(" ms\n"), pserial);
  } else {
    elapsed = elapsed+50;
    pint(elapsed/1000, pserial);
    pserial('.'); pint((elapsed/100)%10, pserial);
    pfstring(PSTR(" s\n"), pserial);
  }
  return bsymbol(NOTHING);
}

object *sp_withoutputtostring (object *args, object *env) {
  object *params = checkarguments(args, 1, 1);
  object *var = first(params);
  object *pair = cons(var, stream(STRINGSTREAM, 0));
  push(pair,env);
  object *string = startstring();
  protect(string);
  object *forms = cdr(args);
  eval(tf_progn(forms,env), env);
  unprotect();
  return string;
}

object *sp_withserial (object *args, object *env) {
  object *params = checkarguments(args, 2, 3);
  object *var = first(params);
  int address = checkinteger(eval(second(params), env));
  params = cddr(params);
  int baud = 96;
  if (params != NULL) baud = checkinteger(eval(first(params), env));
  object *pair = cons(var, stream(SERIALSTREAM, address));
  push(pair,env);
  serialbegin(address, baud);
  object *forms = cdr(args);
  object *result = eval(tf_progn(forms,env), env);
  serialend(address);
  return result;
}

object *sp_withi2c (object *args, object *env) {
  object *params = checkarguments(args, 2, 4);
  object *var = first(params);
  int address = checkinteger(eval(second(params), env));
  params = cddr(params);
  if (address == 0 && params != NULL) params = cdr(params); // Ignore port
  int read = 0; // Write
  I2Ccount = 0;
  if (params != NULL) {
    object *rw = eval(first(params), env);
    if (integerp(rw)) I2Ccount = rw->integer;
    read = (rw != NULL);
  }
  I2Cinit(1); // Pullups
  object *pair = cons(var, (I2Cstart(address, read)) ? stream(I2CSTREAM, address) : nil);
  push(pair,env);
  object *forms = cdr(args);
  object *result = eval(tf_progn(forms,env), env);
  I2Cstop(read);
  return result;
}

object *sp_withspi (object *args, object *env) {
  object *params = checkarguments(args, 2, 6);
  object *var = first(params);
  params = cdr(params);
  if (params == NULL) error2(nostream);
  int pin = checkinteger(eval(car(params), env));
  pinMode(pin, OUTPUT);
  digitalWrite(pin, HIGH);
  params = cdr(params);
  int clock = 4000, mode = SPI_MODE0; // Defaults
  int bitorder = MSBFIRST;
  if (params != NULL) {
    clock = checkinteger(eval(car(params), env));
    params = cdr(params);
    if (params != NULL) {
      bitorder = (checkinteger(eval(car(params), env)) == 0) ? LSBFIRST : MSBFIRST;
      params = cdr(params);
      if (params != NULL) {
        int modeval = checkinteger(eval(car(params), env));
        mode = (modeval == 3) ? SPI_MODE3 : (modeval == 2) ? SPI_MODE2 : (modeval == 1) ? SPI_MODE1 : SPI_MODE0;
      }
    }
  }
  object *pair = cons(var, stream(SPISTREAM, pin));
  push(pair,env);
  SPI.begin();
  SPI.beginTransaction(SPISettings(((unsigned long)clock * 1000), bitorder, mode));
  digitalWrite(pin, LOW);
  object *forms = cdr(args);
  object *result = eval(tf_progn(forms,env), env);
  digitalWrite(pin, HIGH);
  SPI.endTransaction();
  return result;
}

object *sp_withsdcard (object *args, object *env) {
  #if defined(sdcardsupport)
  object *params = checkarguments(args, 2, 3);
  object *var = first(params);
  params = cdr(params);
  if (params == NULL) error2(PSTR("no filename specified"));
  builtin_t temp = Context;
  object *filename = eval(first(params), env);
  Context = temp;
  if (!stringp(filename)) error(PSTR("filename is not a string"), filename);
  params = cdr(params);
  SD.begin(SDCARD_SS_PIN);
  int mode = 0;
  if (params != NULL && first(params) != NULL) mode = checkinteger(first(params));
  int oflag = O_READ;
  if (mode == 1) oflag = O_RDWR | O_CREAT | O_APPEND; else if (mode == 2) oflag = O_RDWR | O_CREAT | O_TRUNC;
  if (mode >= 1) {
    char buffer[BUFFERSIZE];
    SDpfile = SD.open(MakeFilename(filename, buffer), oflag);
    if (!SDpfile) error2(PSTR("problem writing to SD card or invalid filename"));
  } else {
    char buffer[BUFFERSIZE];
    SDgfile = SD.open(MakeFilename(filename, buffer), oflag);
    if (!SDgfile) error2(PSTR("problem reading from SD card or invalid filename"));
  }
  object *pair = cons(var, stream(SDSTREAM, 1));
  push(pair,env);
  object *forms = cdr(args);
  object *result = eval(tf_progn(forms,env), env);
  if (mode >= 1) SDpfile.close(); else SDgfile.close();
  return result;
  #else
  (void) args, (void) env;
  error2(PSTR("not supported"));
  return nil;
  #endif
}

// Assembler

object *sp_defcode (object *args, object *env) {
#if defined(CODESIZE)
  setflag(NOESC);
  object *var = first(args);
  if (!symbolp(var)) error(PSTR("not a symbol"), var);

  // Make *p* a local variable for program counter
  object *pcpair = cons(bsymbol(PSTAR), number(0));
  push(pcpair,env);
  args = cdr(args);

  // Make labels into local variables
  object *entries = cdr(args);
  while (entries != NULL) {
    object *arg = first(entries);
    if (symbolp(arg)) {
      object *pair = cons(arg,number(0));
      push(pair,env);
    }
    entries = cdr(entries);
  }

  // First pass
  int origin = 0;
  int codesize = assemble(1, origin, cdr(args), env, pcpair);

  // See if it will fit
  object *globals = GlobalEnv;
  while (globals != NULL) {
    object *pair = car(globals);
    if (pair != NULL && car(pair) != var && consp(cdr(pair))) { // Exclude me if I already exist
      object *codeid = second(pair);
      if (codeid->type == CODE) {
        codesize = codesize + endblock(codeid) - startblock(codeid);
      }
    }
    globals = cdr(globals);
  }
  if (codesize > CODESIZE) error(PSTR("not enough room for code"), var);

  // Compact the code block, removing gaps
  origin = 0;
  object *block;
  int smallest;

  do {
    smallest = CODESIZE;
    globals = GlobalEnv;
    while (globals != NULL) {
      object *pair = car(globals);
      if (pair != NULL && car(pair) != var && consp(cdr(pair))) { // Exclude me if I already exist
        object *codeid = second(pair);
        if (codeid->type == CODE) {
          if (startblock(codeid) < smallest && startblock(codeid) >= origin) {
            smallest = startblock(codeid);
            block = codeid;
          }
        }
      }
      globals = cdr(globals);
    }

    // Compact fragmentation if necessary
    if (smallest == origin) origin = endblock(block); // No gap
    else if (smallest < CODESIZE) { // Slide block down
      int target = origin;
      for (int i=startblock(block); i<endblock(block); i++) {
        MyCode[target] = MyCode[i];
        target++;
      }
      block->integer = target<<8 | origin;
      origin = target;
    }

  } while (smallest < CODESIZE);

  // Second pass - origin is first free location
  codesize = assemble(2, origin, cdr(args), env, pcpair);

  object *val = cons(codehead((origin+codesize)<<8 | origin), args);
  object *pair = value(var->name, GlobalEnv);
  if (pair != NULL) cdr(pair) = val;
  else push(cons(var, val), GlobalEnv);

  #if defined(CPU_ATmega1284P)
  // Use Optiboot Flasher in MightyCore with 256 byte page from CODE_ADDRESS 0x1bb00 to 0x1bbff
  optiboot_page_erase(CODE_ADDRESS);
  for (unsigned int i=0; i<CODESIZE/2; i++) optiboot_page_fill(CODE_ADDRESS + i*2, MyCode[i*2] | MyCode[i*2+1]<<8);
  optiboot_page_write(CODE_ADDRESS);
  #elif defined (CPU_AVR128DX48)
  // Use Flash Writer in DxCore with 512 byte page from CODE_ADDRESS 0x1be00 to 0x1c000
  if (Flash.checkWritable()) error2(PSTR("flash write not supported"));
  if (Flash.erasePage(CODE_ADDRESS, 1)) error2(PSTR("problem erasing flash"));
  Flash.writeBytes(CODE_ADDRESS, MyCode, CODESIZE);
  #endif

  clrflag(NOESC);
  return var;
#else
  (void) args, (void) env;
  return nil;
#endif
}

// Tail-recursive forms

object *tf_progn (object *args, object *env) {
  if (args == NULL) return nil;
  object *more = cdr(args);
  while (more != NULL) {
    object *result = eval(car(args),env);
    if (tstflag(RETURNFLAG)) return quote(result);
    args = more;
    more = cdr(args);
  }
  return car(args);
}

object *tf_if (object *args, object *env) {
  if (args == NULL || cdr(args) == NULL) error2(toofewargs);
  if (eval(first(args), env) != nil) return second(args);
  args = cddr(args);
  return (args != NULL) ? first(args) : nil;
}

object *tf_cond (object *args, object *env) {
  while (args != NULL) {
    object *clause = first(args);
    if (!consp(clause)) error(illegalclause, clause);
    object *test = eval(first(clause), env);
    object *forms = cdr(clause);
    if (test != nil) {
      if (forms == NULL) return quote(test); else return tf_progn(forms, env);
    }
    args = cdr(args);
  }
  return nil;
}

object *tf_when (object *args, object *env) {
  if (args == NULL) error2(noargument);
  if (eval(first(args), env) != nil) return tf_progn(cdr(args),env);
  else return nil;
}

object *tf_unless (object *args, object *env) {
  if (args == NULL) error2(noargument);
  if (eval(first(args), env) != nil) return nil;
  else return tf_progn(cdr(args),env);
}

object *tf_case (object *args, object *env) {
  object *test = eval(first(args), env);
  args = cdr(args);
  while (args != NULL) {
    object *clause = first(args);
    if (!consp(clause)) error(illegalclause, clause);
    object *key = car(clause);
    object *forms = cdr(clause);
    if (consp(key)) {
      while (key != NULL) {
        if (eq(test,car(key))) return tf_progn(forms, env);
        key = cdr(key);
      }
    } else if (eq(test,key) || eq(key,tee)) return tf_progn(forms, env);
    args = cdr(args);
  }
  return nil;
}

object *tf_and (object *args, object *env) {
  if (args == NULL) return tee;
  object *more = cdr(args);
  while (more != NULL) {
    if (eval(car(args), env) == NULL) return nil;
    args = more;
    more = cdr(args);
  }
  return car(args);
}

// Core functions

object *fn_not (object *args, object *env) {
  (void) env;
  return (first(args) == nil) ? tee : nil;
}

object *fn_cons (object *args, object *env) {
  (void) env;
  return cons(first(args), second(args));
}

object *fn_atom (object *args, object *env) {
  (void) env;
  return atom(first(args)) ? tee : nil;
}

object *fn_listp (object *args, object *env) {
  (void) env;
  return listp(first(args)) ? tee : nil;
}

object *fn_consp (object *args, object *env) {
  (void) env;
  return consp(first(args)) ? tee : nil;
}

object *fn_symbolp (object *args, object *env) {
  (void) env;
  object *arg = first(args);
  return (arg == NULL || symbolp(arg)) ? tee : nil;
}

object *fn_arrayp (object *args, object *env) {
  (void) env;
  return arrayp(first(args)) ? tee : nil;
}

object *fn_boundp (object *args, object *env) {
  return boundp(first(args), env) ? tee : nil;
}

object *fn_keywordp (object *args, object *env) {
  (void) env;
  object *arg = first(args);
  if (!symbolp(arg)) return nil;
  return (keywordp(arg) || colonp(arg->name)) ? tee : nil;
}

object *fn_setfn (object *args, object *env) {
  object *arg = nil;
  while (args != NULL) {
    if (cdr(args) == NULL) error2(oddargs);
    object *pair = findvalue(first(args), env);
    arg = second(args);
    cdr(pair) = arg;
    args = cddr(args);
  }
  return arg;
}

object *fn_streamp (object *args, object *env) {
  (void) env;
  object *arg = first(args);
  return streamp(arg) ? tee : nil;
}

object *fn_eq (object *args, object *env) {
  (void) env;
  return eq(first(args), second(args)) ? tee : nil;
}

object *fn_equal (object *args, object *env) {
  (void) env;
  return equal(first(args), second(args)) ? tee : nil;
}

// List functions

object *fn_car (object *args, object *env) {
  (void) env;
  return carx(first(args));
}

object *fn_cdr (object *args, object *env) {
  (void) env;
  return cdrx(first(args));
}

object *fn_caar (object *args, object *env) {
  (void) env;
  return cxxxr(args, 0b100);
}

object *fn_cadr (object *args, object *env) {
  (void) env;
  return cxxxr(args, 0b101);
}

object *fn_cdar (object *args, object *env) {
  (void) env;
  return cxxxr(args, 0b110);
}

object *fn_cddr (object *args, object *env) {
  (void) env;
  return cxxxr(args, 0b111);
}

object *fn_caaar (object *args, object *env) {
  (void) env;
  return cxxxr(args, 0b1000);
}

object *fn_caadr (object *args, object *env) {
  (void) env;
  return cxxxr(args, 0b1001);;
}

object *fn_cadar (object *args, object *env) {
  (void) env;
  return cxxxr(args, 0b1010);
}

object *fn_caddr (object *args, object *env) {
  (void) env;
  return cxxxr(args, 0b1011);
}

object *fn_cdaar (object *args, object *env) {
  (void) env;
  return cxxxr(args, 0b1100);
}

object *fn_cdadr (object *args, object *env) {
  (void) env;
  return cxxxr(args, 0b1101);
}

object *fn_cddar (object *args, object *env) {
  (void) env;
  return cxxxr(args, 0b1110);
}

object *fn_cdddr (object *args, object *env) {
  (void) env;
  return cxxxr(args, 0b1111);
}

object *fn_length (object *args, object *env) {
  (void) env;
  object *arg = first(args);
  if (listp(arg)) return number(listlength(arg));
  if (stringp(arg)) return number(stringlength(arg));
  if (!(arrayp(arg) && cdr(cddr(arg)) == NULL)) error(PSTR("argument is not a list, 1d array, or string"), arg);
  return number(abs(first(cddr(arg))->integer));
}

object *fn_arraydimensions (object *args, object *env) {
  (void) env;
  object *array = first(args);
  if (!arrayp(array)) error(PSTR("argument is not an array"), array);
  object *dimensions = cddr(array);
  return (first(dimensions)->integer < 0) ? cons(number(-(first(dimensions)->integer)), cdr(dimensions)) : dimensions;
}

object *fn_list (object *args, object *env) {
  (void) env;
  return args;
}

object *fn_copylist (object *args, object *env) {
  (void) env;
  object *arg = first(args);
  if (!listp(arg)) error(notalist, arg);
  object *result = cons(NULL, NULL);
  object *ptr = result;
  while (arg != NULL) {
    cdr(ptr) = cons(car(arg), NULL);
    ptr = cdr(ptr); arg = cdr(arg);
  }
  return cdr(result);
}

object *fn_makearray (object *args, object *env) {
  (void) env;
  object *def = nil;
  bool bitp = false;
  object *dims = first(args);
  if (dims == NULL) error2(PSTR("dimensions can't be nil"));
  else if (atom(dims)) dims = cons(dims, NULL);
  args = cdr(args);
  while (args != NULL && cdr(args) != NULL) {
    object *var = first(args);
    if (isbuiltin(first(args), INITIALELEMENT)) def = second(args);
    else if (isbuiltin(first(args), ELEMENTTYPE) && isbuiltin(second(args), BIT)) bitp = true;
    else error(PSTR("argument not recognised"), var);
    args = cddr(args);
  }
  if (bitp) {
    if (def == nil) def = number(0);
    else def = number(-checkbitvalue(def)); // 1 becomes all ones
  }
  return makearray(dims, def, bitp);
}

object *fn_reverse (object *args, object *env) {
  (void) env;
  object *list = first(args);
  object *result = NULL;
  while (list != NULL) {
    if (improperp(list)) error(notproper, list);
    push(first(list),result);
    list = cdr(list);
  }
  return result;
}

object *fn_nth (object *args, object *env) {
  (void) env;
  int n = checkinteger(first(args));
  if (n < 0) error(indexnegative, first(args));
  object *list = second(args);
  while (list != NULL) {
    if (improperp(list)) error(notproper, list);
    if (n == 0) return car(list);
    list = cdr(list);
    n--;
  }
  return nil;
}

object *fn_aref (object *args, object *env) {
  (void) env;
  int bit;
  object *array = first(args);
  if (!arrayp(array)) error(PSTR("first argument is not an array"), array);
  object *loc = *getarray(array, cdr(args), 0, &bit);
  if (bit == -1) return loc;
  else return number((loc->integer)>>bit & 1);
}

object *fn_assoc (object *args, object *env) {
  (void) env;
  object *key = first(args);
  object *list = second(args);
  object *test = testargument(cddr(args));
  while (list != NULL) {
    if (improperp(list)) error(notproper, list);
    object *pair = first(list);
    if (!listp(pair)) error(PSTR("element is not a list"), pair);
    if (pair != NULL && apply(test, cons(key, cons(car(pair), NULL)), env) != NULL) return pair;
    list = cdr(list);
  }
  return nil;
}

object *fn_member (object *args, object *env) {
  (void) env;
  object *item = first(args);
  object *list = second(args);
  object *test = testargument(cddr(args));
  while (list != NULL) {
    if (improperp(list)) error(notproper, list);
    if (apply(test, cons(item, cons(car(list), NULL)), env) != NULL) return list;
    list = cdr(list);
  }
  return nil;
}

object *fn_apply (object *args, object *env) {
  object *previous = NULL;
  object *last = args;
  while (cdr(last) != NULL) {
    previous = last;
    last = cdr(last);
  }
  object *arg = car(last);
  if (!listp(arg)) error(notalist, arg);
  cdr(previous) = arg;
  return apply(first(args), cdr(args), env);
}

object *fn_funcall (object *args, object *env) {
  return apply(first(args), cdr(args), env);
}

object *fn_append (object *args, object *env) {
  (void) env;
  object *head = NULL;
  object *tail;
  while (args != NULL) {
    object *list = first(args);
    if (!listp(list)) error(notalist, list);
    while (consp(list)) {
      object *obj = cons(car(list), cdr(list));
      if (head == NULL) head = obj;
      else cdr(tail) = obj;
      tail = obj;
      list = cdr(list);
      if (cdr(args) != NULL && improperp(list)) error(notproper, first(args));
    }
    args = cdr(args);
  }
  return head;
}

object *fn_mapc (object *args, object *env) {
  return mapcl(args, env, false);
}

object *fn_mapl (object *args, object *env) {
  return mapcl(args, env, true);
}

object *fn_mapcar (object *args, object *env) {
  return mapcarcan(args, env, mapcarfun, false);
}

object *fn_mapcan (object *args, object *env) {
  return mapcarcan(args, env, mapcanfun, false);
}

object *fn_maplist (object *args, object *env) {
  return mapcarcan(args, env, mapcarfun, true);
}

object *fn_mapcon (object *args, object *env) {
  return mapcarcan(args, env, mapcanfun, true);
}

// Arithmetic functions

object *fn_add (object *args, object *env) {
  (void) env;
  int result = 0;
  while (args != NULL) {
    int temp = checkinteger(car(args));
    #if defined(checkoverflow)
    if (temp < 1) { if (INT_MIN - temp > result) error2(overflow); }
    else { if (INT_MAX - temp < result) error2(overflow); }
    #endif
    result = result + temp;
    args = cdr(args);
  }
  return number(result);
}

object *fn_subtract (object *args, object *env) {
  (void) env;
  int result = checkinteger(car(args));
  args = cdr(args);
  if (args == NULL) {
    #if defined(checkoverflow)
    if (result == INT_MIN) error2(overflow);
    #endif
    return number(-result);
  }
  while (args != NULL) {
    int temp = checkinteger(car(args));
    #if defined(checkoverflow)
    if (temp < 1) { if (INT_MAX + temp < result) error2(overflow); }
    else { if (INT_MIN + temp > result) error2(overflow); }
    #endif
    result = result - temp;
    args = cdr(args);
  }
  return number(result);
}

object *fn_multiply (object *args, object *env) {
  (void) env;
  int result = 1;
  while (args != NULL){
    #if defined(checkoverflow)
    signed long temp = (signed long) result * checkinteger(car(args));
    if ((temp > INT_MAX) || (temp < INT_MIN)) error2(overflow);
    result = temp;
    #else
    result = result * checkinteger(car(args));
    #endif
    args = cdr(args);
  }
  return number(result);
}

object *fn_divide (object *args, object *env) {
  (void) env;
  int result = checkinteger(first(args));
  args = cdr(args);
  while (args != NULL) {
    int arg = checkinteger(car(args));
    if (arg == 0) error2(divisionbyzero);
    #if defined(checkoverflow)
    if ((result == INT_MIN) && (arg == -1)) error2(overflow);
    #endif
    result = result / arg;
    args = cdr(args);
  }
  return number(result);
}

object *fn_mod (object *args, object *env) {
  (void) env;
  int arg1 = checkinteger(first(args));
  int arg2 = checkinteger(second(args));
  if (arg2 == 0) error2(divisionbyzero);
  int r = arg1 % arg2;
  if ((arg1<0) != (arg2<0)) r = r + arg2;
  return number(r);
}

object *fn_oneplus (object *args, object *env) {
  (void) env;
  int result = checkinteger(first(args));
  #if defined(checkoverflow)
  if (result == INT_MAX) error2(overflow);
  #endif
  return number(result + 1);
}

object *fn_oneminus (object *args, object *env) {
  (void) env;
  int result = checkinteger(first(args));
  #if defined(checkoverflow)
  if (result == INT_MIN) error2(overflow);
  #endif
  return number(result - 1);
}

object *fn_abs (object *args, object *env) {
  (void) env;
  int result = checkinteger(first(args));
  #if defined(checkoverflow)
  if (result == INT_MIN) error2(overflow);
  #endif
  return number(abs(result));
}

object *fn_random (object *args, object *env) {
  (void) env;
  int arg = checkinteger(first(args));
  return number(pseudoRandom(arg));
}

object *fn_maxfn (object *args, object *env) {
  (void) env;
  int result = checkinteger(first(args));
  args = cdr(args);
  while (args != NULL) {
    int next = checkinteger(car(args));
    if (next > result) result = next;
    args = cdr(args);
  }
  return number(result);
}

object *fn_minfn (object *args, object *env) {
  (void) env;
  int result = checkinteger(first(args));
  args = cdr(args);
  while (args != NULL) {
    int next = checkinteger(car(args));
    if (next < result) result = next;
    args = cdr(args);
  }
  return number(result);
}

// Arithmetic comparisons

object *fn_noteq (object *args, object *env) {
  (void) env;
  while (args != NULL) {
    object *nargs = args;
    int arg1 = checkinteger(first(nargs));
    nargs = cdr(nargs);
    while (nargs != NULL) {
       int arg2 = checkinteger(first(nargs));
       if (arg1 == arg2) return nil;
       nargs = cdr(nargs);
    }
    args = cdr(args);
  }
  return tee;
}

object *fn_numeq (object *args, object *env) {
  (void) env;
  return compare(args, false, false, true);
}

object *fn_less (object *args, object *env) {
  (void) env;
  return compare(args, true, false, false);
}

object *fn_lesseq (object *args, object *env) {
  (void) env;
  return compare(args, true, false, true);
}

object *fn_greater (object *args, object *env) {
  (void) env;
  return compare(args, false, true, false);
}

object *fn_greatereq (object *args, object *env) {
  (void) env;
  return compare(args, false, true, true);
}

object *fn_plusp (object *args, object *env) {
  (void) env;
  int arg = checkinteger(first(args));
  if (arg > 0) return tee;
  else return nil;
}

object *fn_minusp (object *args, object *env) {
  (void) env;
  int arg = checkinteger(first(args));
  if (arg < 0) return tee;
  else return nil;
}

object *fn_zerop (object *args, object *env) {
  (void) env;
  int arg = checkinteger(first(args));
  return (arg == 0) ? tee : nil;
}

object *fn_oddp (object *args, object *env) {
  (void) env;
  int arg = checkinteger(first(args));
  return ((arg & 1) == 1) ? tee : nil;
}

object *fn_evenp (object *args, object *env) {
  (void) env;
  int arg = checkinteger(first(args));
  return ((arg & 1) == 0) ? tee : nil;
}

// Number functions

object *fn_integerp (object *args, object *env) {
  (void) env;
  return integerp(first(args)) ? tee : nil;
}

// Characters

object *fn_char (object *args, object *env) {
  (void) env;
  object *arg = first(args);
  if (!stringp(arg)) error(notastring, arg);
  object *n = second(args);
  char c = nthchar(arg, checkinteger(n));
  if (c == 0) error(indexrange, n);
  return character(c);
}

object *fn_charcode (object *args, object *env) {
  (void) env;
  return number(checkchar(first(args)));
}

object *fn_codechar (object *args, object *env) {
  (void) env;
  return character(checkinteger(first(args)));
}

object *fn_characterp (object *args, object *env) {
  (void) env;
  return characterp(first(args)) ? tee : nil;
}

// Strings

object *fn_stringp (object *args, object *env) {
  (void) env;
  return stringp(first(args)) ? tee : nil;
}

object *fn_stringeq (object *args, object *env) {
  (void) env;
  int m = stringcompare(args, false, false, true);
  return m == -1 ? nil : tee;
}

object *fn_stringless (object *args, object *env) {
  (void) env;
  int m = stringcompare(args, true, false, false);
  return m == -1 ? nil : number(m);
}

object *fn_stringgreater (object *args, object *env) {
  (void) env;
  int m = stringcompare(args, false, true, false);
  return m == -1 ? nil : number(m);
}

object *fn_stringnoteq (object *args, object *env) {
  (void) env;
  int m = stringcompare(args, true, true, false);
  return m == -1 ? nil : number(m);
}

object *fn_stringlesseq (object *args, object *env) {
  (void) env;
  int m = stringcompare(args, true, false, true);
  return m == -1 ? nil : number(m);
}

object *fn_stringgreatereq (object *args, object *env) {
  (void) env;
  int m = stringcompare(args, false, true, true);
  return m == -1 ? nil : number(m);
}

object *fn_sort (object *args, object *env) {
  if (first(args) == NULL) return nil;
  object *list = cons(nil,first(args));
  protect(list);
  object *predicate = second(args);
  object *compare = cons(NULL, cons(NULL, NULL));
  protect(compare);
  object *ptr = cdr(list);
  while (cdr(ptr) != NULL) {
    object *go = list;
    while (go != ptr) {
      car(compare) = car(cdr(ptr));
      car(cdr(compare)) = car(cdr(go));
      if (apply(predicate, compare, env)) break;
      go = cdr(go);
    }
    if (go != ptr) {
      object *obj = cdr(ptr);
      cdr(ptr) = cdr(obj);
      cdr(obj) = cdr(go);
      cdr(go) = obj;
    } else ptr = cdr(ptr);
  }
  unprotect(); unprotect();
  return cdr(list);
}

object *fn_stringfn (object *args, object *env) {
  return fn_princtostring(args, env);
}

object *fn_concatenate (object *args, object *env) {
  (void) env;
  object *arg = first(args);
  if (builtin(arg->name) != STRINGFN) error2(PSTR("only supports strings"));
  args = cdr(args);
  object *result = newstring();
  object *tail = result;
  while (args != NULL) {
    object *obj = checkstring(first(args));
    obj = cdr(obj);
    while (obj != NULL) {
      int quad = obj->chars;
      while (quad != 0) {
         char ch = quad>>((sizeof(int)-1)*8) & 0xFF;
         buildstring(ch, &tail);
         quad = quad<<8;
      }
      obj = car(obj);
    }
    args = cdr(args);
  }
  return result;
}

object *fn_subseq (object *args, object *env) {
  (void) env;
  object *arg = first(args);
  int start = checkinteger(second(args)), end;
  if (start < 0) error(indexnegative, second(args));
  args = cddr(args);
  if (listp(arg)) {
    int length = listlength(arg);
    if (args != NULL) end = checkinteger(car(args)); else end = length;
    if (start > end || end > length) error2(indexrange);
    object *result = cons(NULL, NULL);
    object *ptr = result;
    for (int x = 0; x < end; x++) {
      if (x >= start) { cdr(ptr) = cons(car(arg), NULL); ptr = cdr(ptr); }
      arg = cdr(arg);
    }
    return cdr(result);
  } else if (stringp(arg)) {
    int length = stringlength(arg);
    if (args != NULL) end = checkinteger(car(args)); else end = length;
    if (start > end || end > length) error2(indexrange);
    object *result = newstring();
    object *tail = result;
    for (int i=start; i<end; i++) {
      char ch = nthchar(arg, i);
      buildstring(ch, &tail);
    }
    return result;
  } else error2(PSTR("argument is not a list or string"));
  return nil;
}

object *fn_search (object *args, object *env) {
  (void) env;
  object *pattern = first(args);
  object *target = second(args);
  if (pattern == NULL) return number(0);
  else if (target == NULL) return nil;

  else if (listp(pattern) && listp(target)) {
    object *test = testargument(cddr(args));
    int l = listlength(target);
    int m = listlength(pattern);
    for (int i = 0; i <= l-m; i++) {
      object *target1 = target;
      while (pattern != NULL && apply(test, cons(car(target1), cons(car(pattern), NULL)), env) != NULL) {
        pattern = cdr(pattern);
        target1 = cdr(target1);
      }
      if (pattern == NULL) return number(i);
      pattern = first(args); target = cdr(target);
    }
    return nil;

  } else if (stringp(pattern) && stringp(target)) {
    if (cddr(args) != NULL) error2(PSTR("keyword argument not supported for strings"));
    int l = stringlength(target);
    int m = stringlength(pattern);
    for (int i = 0; i <= l-m; i++) {
      int j = 0;
      while (j < m && nthchar(target, i+j) == nthchar(pattern, j)) j++;
      if (j == m) return number(i);
    }
    return nil;
  } else error2(PSTR("arguments are not both lists or strings"));
  return nil;
}

object *fn_readfromstring (object *args, object *env) {
  (void) env;
  object *arg = checkstring(first(args));
  GlobalString = arg;
  GlobalStringIndex = 0;
  object *val = read(gstr);
  LastChar = 0;
  return val;
}

object *fn_princtostring (object *args, object *env) {
  (void) env;
  return princtostring(first(args));
}

object *fn_prin1tostring (object *args, object *env) {
  (void) env;
  object *arg = first(args);
  object *obj = startstring();
  printobject(arg, pstr);
  return obj;
}

// Bitwise operators

object *fn_logand (object *args, object *env) {
  (void) env;
  int result = -1;
  while (args != NULL) {
    result = result & checkinteger(first(args));
    args = cdr(args);
  }
  return number(result);
}

object *fn_logior (object *args, object *env) {
  (void) env;
  int result = 0;
  while (args != NULL) {
    result = result | checkinteger(first(args));
    args = cdr(args);
  }
  return number(result);
}

object *fn_logxor (object *args, object *env) {
  (void) env;
  int result = 0;
  while (args != NULL) {
    result = result ^ checkinteger(first(args));
    args = cdr(args);
  }
  return number(result);
}

object *fn_lognot (object *args, object *env) {
  (void) env;
  int result = checkinteger(car(args));
  return number(~result);
}

object *fn_ash (object *args, object *env) {
  (void) env;
  int value = checkinteger(first(args));
  int count = checkinteger(second(args));
  if (count >= 0) return number(value << count);
  else return number(value >> abs(count));
}

object *fn_logbitp (object *args, object *env) {
  (void) env;
  int index = checkinteger(first(args));
  int value = checkinteger(second(args));
  return (bitRead(value, index) == 1) ? tee : nil;
}

// System functions

object *fn_eval (object *args, object *env) {
  return eval(first(args), env);
}

object *fn_return (object *args, object *env) {
  (void) env;
  setflag(RETURNFLAG);
  if (args == NULL) return nil; else return first(args);
}

object *fn_globals (object *args, object *env) {
  (void) args, (void) env;
  object *result = cons(NULL, NULL);
  object *ptr = result;
  object *arg = GlobalEnv;
  while (arg != NULL) {
    cdr(ptr) = cons(car(car(arg)), NULL); ptr = cdr(ptr);
    arg = cdr(arg);
  }
  return cdr(result);
}

object *fn_locals (object *args, object *env) {
  (void) args;
  return env;
}

object *fn_makunbound (object *args, object *env) {
  (void) env;
  object *var = first(args);
  if (!symbolp(var)) error(notasymbol, var);
  delassoc(var, &GlobalEnv);
  return var;
}

object *fn_break (object *args, object *env) {
  (void) args;
  pfstring(PSTR("\nBreak!\n"), pserial);
  BreakLevel++;
  repl(env);
  BreakLevel--;
  return nil;
}

object *fn_read (object *args, object *env) {
  (void) env;
  gfun_t gfun = gstreamfun(args);
  return read(gfun);
}

object *fn_prin1 (object *args, object *env) {
  (void) env;
  object *obj = first(args);
  pfun_t pfun = pstreamfun(cdr(args));
  printobject(obj, pfun);
  return obj;
}

object *fn_print (object *args, object *env) {
  (void) env;
  object *obj = first(args);
  pfun_t pfun = pstreamfun(cdr(args));
  pln(pfun);
  printobject(obj, pfun);
  pfun(' ');
  return obj;
}

object *fn_princ (object *args, object *env) {
  (void) env;
  object *obj = first(args);
  pfun_t pfun = pstreamfun(cdr(args));
  prin1object(obj, pfun);
  return obj;
}

object *fn_terpri (object *args, object *env) {
  (void) env;
  pfun_t pfun = pstreamfun(args);
  pln(pfun);
  return nil;
}

object *fn_readbyte (object *args, object *env) {
  (void) env;
  gfun_t gfun = gstreamfun(args);
  int c = gfun();
  return (c == -1) ? nil : number(c);
}

object *fn_readline (object *args, object *env) {
  (void) env;
  gfun_t gfun = gstreamfun(args);
  return readstring('\n', false, gfun);
}

object *fn_writebyte (object *args, object *env) {
  (void) env;
  int value = checkinteger(first(args));
  pfun_t pfun = pstreamfun(cdr(args));
  (pfun)(value);
  return nil;
}

object *fn_writestring (object *args, object *env) {
  (void) env;
  object *obj = first(args);
  pfun_t pfun = pstreamfun(cdr(args));
  char temp = Flags;
  clrflag(PRINTREADABLY);
  printstring(obj, pfun);
  Flags = temp;
  return nil;
}

object *fn_writeline (object *args, object *env) {
  (void) env;
  object *obj = first(args);
  pfun_t pfun = pstreamfun(cdr(args));
  char temp = Flags;
  clrflag(PRINTREADABLY);
  printstring(obj, pfun);
  pln(pfun);
  Flags = temp;
  return nil;
}

object *fn_restarti2c (object *args, object *env) {
  (void) env;
  int stream = isstream(first(args));
  args = cdr(args);
  int read = 0; // Write
  I2Ccount = 0;
  if (args != NULL) {
    object *rw = first(args);
    if (integerp(rw)) I2Ccount = rw->integer;
    read = (rw != NULL);
  }
  int address = stream & 0xFF;
  if (stream>>8 != I2CSTREAM) error2(PSTR("not an i2c stream"));
  return I2Crestart(address, read) ? tee : nil;
}

object *fn_gc (object *obj, object *env) {
  int initial = Freespace;
  unsigned long start = micros();
  gc(obj, env);
  unsigned long elapsed = micros() - start;
  pfstring(PSTR("Space: "), pserial);
  pint(Freespace - initial, pserial);
  pfstring(PSTR(" bytes, Time: "), pserial);
  pint(elapsed, pserial);
  pfstring(PSTR(" us\n"), pserial);
  return nil;
}

object *fn_room (object *args, object *env) {
  (void) args, (void) env;
  return number(Freespace);
}

object *fn_saveimage (object *args, object *env) {
  if (args != NULL) args = eval(first(args), env);
  return number(saveimage(args));
}

object *fn_loadimage (object *args, object *env) {
  (void) env;
  if (args != NULL) args = first(args);
  return number(loadimage(args));
}

object *fn_cls (object *args, object *env) {
  (void) args, (void) env;
  pserial(12);
  return nil;
}

// Arduino procedures

object *fn_pinmode (object *args, object *env) {
  (void) env; int pin;
  object *arg = first(args);
  if (keywordp(arg)) pin = checkkeyword(arg);
  else pin = checkinteger(first(args));
  int pm = INPUT;
  arg = second(args);
  if (keywordp(arg)) pm = checkkeyword(arg);
  else if (integerp(arg)) {
    int mode = arg->integer;
    if (mode == 1) pm = OUTPUT; else if (mode == 2) pm = INPUT_PULLUP;
    #if defined(INPUT_PULLDOWN)
    else if (mode == 4) pm = INPUT_PULLDOWN;
    #endif
  } else if (arg != nil) pm = OUTPUT;
  pinMode(pin, pm);
  return nil;
}

object *fn_digitalread (object *args, object *env) {
  (void) env;
  int pin;
  object *arg = first(args);
  if (keywordp(arg)) pin = checkkeyword(arg);
  else pin = checkinteger(arg);
  if (digitalRead(pin) != 0) return tee; else return nil;
}

object *fn_digitalwrite (object *args, object *env) {
  (void) env;
  int pin;
  object *arg = first(args);
  if (keywordp(arg)) pin = checkkeyword(arg);
  else pin = checkinteger(arg);
  arg = second(args);
  int mode;
  if (keywordp(arg)) mode = checkkeyword(arg);
  else if (integerp(arg)) mode = arg->integer ? HIGH : LOW;
  else mode = (arg != nil) ? HIGH : LOW;
  digitalWrite(pin, mode);
  return arg;
}

object *fn_analogread (object *args, object *env) {
  (void) env;
  int pin;
  object *arg = first(args);
  if (keywordp(arg)) pin = checkkeyword(arg);
  else {
    pin = checkinteger(arg);
    checkanalogread(pin);
  }
  return number(analogRead(pin));
}

object *fn_analogreference (object *args, object *env) {
  (void) env;
  object *arg = first(args);
  analogReference(checkkeyword(arg));
  return arg;
}

object *fn_analogreadresolution (object *args, object *env) {
  (void) env;
  object *arg = first(args);
  #if defined(CPU_AVR128DX48)
  uint8_t res = checkinteger(arg);
  if (res == 10) analogReadResolution(10);
  else if (res == 12) analogReadResolution(12);
  else error(PSTR("invalid resolution"), arg);
  #else
  error2(PSTR("not supported"));
  #endif
  return arg;
}

object *fn_analogwrite (object *args, object *env) {
  (void) env;
  int pin;
  object *arg = first(args);
  if (keywordp(arg)) pin = checkkeyword(arg);
  else pin = checkinteger(arg);
  checkanalogwrite(pin);
  object *value = second(args);
  analogWrite(pin, checkinteger(value));
  return value;
}

object *fn_dacreference (object *args, object *env) {
  (void) env;
  object *arg = first(args);
  #if defined(CPU_AVR128DX48)
  int ref = checkinteger(arg);
  DACReference(ref);
  #endif
  return arg;
}

object *fn_delay (object *args, object *env) {
  (void) env;
  object *arg1 = first(args);
  unsigned long start = millis();
  unsigned long total = checkinteger(arg1);
  do testescape();
  while (millis() - start < total);
  return arg1;
}

object *fn_millis (object *args, object *env) {
  (void) args, (void) env;
  return number(millis());
}

object *fn_sleep (object *args, object *env) {
  (void) env;
  if (args == NULL || first(args) == NULL) { sleep(); return nil; }
  object *arg1 = first(args);
  doze(checkinteger(arg1));
  return arg1;
}

object *fn_note (object *args, object *env) {
  (void) env;
  static int pin = 255;
  if (args != NULL) {
    pin = checkinteger(first(args));
    int note = 48, octave = 0;
    if (cdr(args) != NULL) {
      note = checkinteger(second(args));
      if (cddr(args) != NULL) octave = checkinteger(third(args));
    }
    playnote(pin, note, octave);
  } else nonote(pin);
  return nil;
}

object *fn_register (object *args, object *env) {
  (void) env;
  object *arg = first(args);
  int addr;
  if (keywordp(arg)) addr = checkkeyword(arg);
  else addr = checkinteger(first(args));
  if (cdr(args) == NULL) return number(*(volatile uint8_t *)addr);
  (*(volatile uint8_t *)addr) = checkinteger(second(args));
  return second(args);
}

// Tree Editor

object *fn_edit (object *args, object *env) {
  object *fun = first(args);
  object *pair = findvalue(fun, env);
  clrflag(EXITEDITOR);
  object *arg = edit(eval(fun, env));
  cdr(pair) = arg;
  return arg;
}

// Pretty printer

object *fn_pprint (object *args, object *env) {
  (void) env;
  object *obj = first(args);
  pfun_t pfun = pstreamfun(cdr(args));
  pln(pfun);
  superprint(obj, 0, pfun);
  return bsymbol(NOTHING);
}

object *fn_pprintall (object *args, object *env) {
  (void) env;
  pfun_t pfun = pstreamfun(args);
  object *globals = GlobalEnv;
  while (globals != NULL) {
    object *pair = first(globals);
    object *var = car(pair);
    object *val = cdr(pair);
    pln(pfun);
    if (consp(val) && symbolp(car(val)) && builtin(car(val)->name) == LAMBDA) {
      superprint(cons(bsymbol(DEFUN), cons(var, cdr(val))), 0, pfun);
    #if defined(CODESIZE)
    } else if (consp(val) && car(val)->type == CODE) {
      superprint(cons(bsymbol(DEFCODE), cons(var, cdr(val))), 0, pfun);
    #endif
    } else {
      superprint(cons(bsymbol(DEFVAR), cons(var, cons(quote(val), NULL))), 0, pfun);
    }
    pln(pfun);
    testescape();
    globals = cdr(globals);
  }
  return bsymbol(NOTHING);
}

// Format

object *fn_format (object *args, object *env) {
  (void) env;
  pfun_t pfun = pserial;
  object *output = first(args);
  object *obj;
  if (output == nil) { obj = startstring(); pfun = pstr; }
  else if (output != tee) pfun = pstreamfun(args);
  object *formatstr = checkstring(second(args));
  object *save = NULL;
  args = cddr(args);
  int len = stringlength(formatstr);
  uint8_t n = 0, width = 0, w, bra = 0;
  char pad = ' ';
  bool tilde = false, mute = false, comma = false, quote = false;
  while (n < len) {
    char ch = nthchar(formatstr, n);
    char ch2 = ch & ~0x20; // force to upper case
    if (tilde) {
     if (ch == '}') {
        if (save == NULL) formaterr(formatstr, PSTR("no matching ~{"), n);
        if (args == NULL) { args = cdr(save); save = NULL; } else n = bra;
        mute = false; tilde = false;
      }
      else if (!mute) {
        if (comma && quote) { pad = ch; comma = false, quote = false; }
        else if (ch == '\'') {
          if (comma) quote = true;
          else formaterr(formatstr, PSTR("quote not valid"), n);
        }
        else if (ch == '~') { pfun('~'); tilde = false; }
        else if (ch >= '0' && ch <= '9') width = width*10 + ch - '0';
        else if (ch == ',') comma = true;
        else if (ch == '%') { pln(pfun); tilde = false; }
        else if (ch == '&') { pfl(pfun); tilde = false; }
        else if (ch == '^') {
          if (save != NULL && args == NULL) mute = true;
          tilde = false;
        }
        else if (ch == '{') {
          if (save != NULL) formaterr(formatstr, PSTR("can't nest ~{"), n);
          if (args == NULL) formaterr(formatstr, noargument, n);
          if (!listp(first(args))) formaterr(formatstr, notalist, n);
          save = args; args = first(args); bra = n; tilde = false;
          if (args == NULL) mute = true;
        }
        else if (ch2 == 'A' || ch2 == 'S' || ch2 == 'D' || ch2 == 'G' || ch2 == 'X' || ch2 == 'B') {
          if (args == NULL) formaterr(formatstr, noargument, n);
          object *arg = first(args); args = cdr(args);
          uint8_t aw = atomwidth(arg);
          if (width < aw) w = 0; else w = width-aw;
          tilde = false;
          if (ch2 == 'A') { prin1object(arg, pfun); indent(w, pad, pfun); }
          else if (ch2 == 'S') { printobject(arg, pfun); indent(w, pad, pfun); }
          else if (ch2 == 'D' || ch2 == 'G') { indent(w, pad, pfun); prin1object(arg, pfun); }
          else if (ch2 == 'X' || ch2 == 'B') {
            if (integerp(arg)) {
              uint8_t base = (ch2 == 'B') ? 2 : 16;
              uint8_t hw = basewidth(arg, base); if (width < hw) w = 0; else w = width-hw;
              indent(w, pad, pfun); pintbase(arg->integer, base, pfun);
            } else {
              indent(w, pad, pfun); prin1object(arg, pfun);
            }
          }
          tilde = false;
        } else formaterr(formatstr, PSTR("invalid directive"), n);
      }
    } else {
      if (ch == '~') { tilde = true; pad = ' '; width = 0; comma = false; quote = false; }
      else if (!mute) pfun(ch);
    }
    n++;
  }
  if (output == nil) return obj;
  else return nil;
}

// LispLibrary

object *fn_require (object *args, object *env) {
  object *arg = first(args);
  object *globals = GlobalEnv;
  if (!symbolp(arg)) error(notasymbol, arg);
  while (globals != NULL) {
    object *pair = first(globals);
    object *var = car(pair);
    if (symbolp(var) && var == arg) return nil;
    globals = cdr(globals);
  }
  GlobalStringIndex = 0;
  object *line = read(glibrary);
  while (line != NULL) {
    // Is this the definition we want
    symbol_t fname = first(line)->name;
    if ((fname == sym(DEFUN) || fname == sym(DEFVAR)) && symbolp(second(line)) && second(line)->name == arg->name) {
      eval(line, env);
      return tee;
    }
    line = read(glibrary);
  }
  return nil;
}

object *fn_listlibrary (object *args, object *env) {
  (void) args, (void) env;
  GlobalStringIndex = 0;
  object *line = read(glibrary);
  while (line != NULL) {
    builtin_t bname = builtin(first(line)->name);
    if (bname == DEFUN || bname == DEFVAR) {
      printsymbol(second(line), pserial); pserial(' ');
    }
    line = read(glibrary);
  }
  return bsymbol(NOTHING);
}

// Documentation

object *sp_help (object *args, object *env) {
  if (args == NULL) error2(noargument);
  object *docstring = documentation(first(args), env);
  if (docstring) {
    char temp = Flags;
    clrflag(PRINTREADABLY);
    printstring(docstring, pserial);
    Flags = temp;
  }
  return bsymbol(NOTHING);
}

object *fn_documentation (object *args, object *env) {
  return documentation(first(args), env);
}

object *fn_apropos (object *args, object *env) {
  (void) env;
  apropos(first(args), true);
  return bsymbol(NOTHING);
}

object *fn_aproposlist (object *args, object *env) {
  (void) env;
  return apropos(first(args), false);
}

// Graphics functions

const uint8_t xsize = 250, ysize = 122, ymax = 121;

object *fn_drawpixel (object *args, object *env) {
  (void) env;
  uint8_t colour = 1;
  if (cddr(args) != NULL) colour = checkinteger(third(args));
  PlotPoint(checkinteger(first(args)), ymax-checkinteger(second(args)), colour);
  PlotPoint(255, 0, colour); // Flush
  return nil;
}

object *fn_drawline (object *args, object *env) {
  (void) env;
  uint8_t colour = 1;
  uint16_t params[4];
  for (int i=0; i<4; i++) { params[i] = checkinteger(car(args)); args = cdr(args); }
  if (args != NULL) colour = checkinteger(car(args));
  MoveTo(params[0], ymax-params[1]);
  DrawTo(params[2], ymax-params[3], colour);
  return nil;
}

object *fn_drawrect (object *args, object *env) {
  (void) env;
  uint8_t colour = 1;
  uint16_t params[4];
  for (int i=0; i<4; i++) { params[i] = checkinteger(car(args)); args = cdr(args); }
  if (args != NULL) colour = checkinteger(car(args));
  MoveTo(params[0], ymax-params[1]);
  DrawRect(params[2]-params[0], params[3]-params[1], colour);
  return nil;
}

object *fn_fillrect (object *args, object *env) {
  (void) env;
  uint8_t colour = 1;
  uint16_t params[4];
  for (int i=0; i<4; i++) { params[i] = checkinteger(car(args)); args = cdr(args); }
  if (args != NULL) colour = checkinteger(car(args));
  MoveTo(params[0], ymax-params[1]);
  FillRect(params[2]-params[0], params[3]-params[1], colour);
  return nil;
}

object *fn_drawcircle (object *args, object *env) {
  (void) env;
  uint8_t colour = 1;
  uint16_t params[3];
  for (int i=0; i<3; i++) { params[i] = checkinteger(car(args)); args = cdr(args); }
  if (args != NULL) colour = checkinteger(car(args));
  MoveTo(params[0], ymax-params[1]);
  DrawCircle(params[2], colour);
  return nil;
}

object *fn_fillcircle (object *args, object *env) {
  (void) env;
  uint8_t colour = 1;
  uint16_t params[3];
  for (int i=0; i<3; i++) { params[i] = checkinteger(car(args)); args = cdr(args); }
  if (args != NULL) colour = checkinteger(car(args));
  MoveTo(params[0], ymax-params[1]);
  FillCircle(params[2], colour);
  return nil;
}

object *fn_drawtriangle (object *args, object *env) {
  (void) env;
  uint8_t colour = 1;
  uint16_t params[6];
  for (int i=0; i<6; i++) { params[i] = checkinteger(car(args)); args = cdr(args); }
  if (args != NULL) colour = checkinteger(car(args));
  DrawTriangle(params[0], params[1], params[2], params[3], params[4], params[5], colour);
  return nil;
}

object *fn_filltriangle (object *args, object *env) {
  (void) env;
  uint8_t colour = 1;
  uint16_t params[6];
  for (int i=0; i<6; i++) { params[i] = checkinteger(car(args)); args = cdr(args); }
  if (args != NULL) colour = checkinteger(car(args));
  FillTriangle(params[0], params[1], params[2], params[3], params[4], params[5], colour);
  return nil;
}

object *fn_fillscreen (object *args, object *env) {
  (void) env;
  uint8_t grey = 0;
  if (args != NULL) grey = checkinteger(first(args));
  ClearDisplay(grey);
  return nil;
}

object *fn_getpixel (object *args, object *env) {
  (void) env;
  uint8_t x = checkinteger(first(args));
  uint8_t y = ymax - checkinteger(second(args));
  uint8_t v = 121 - y;
  uint8_t row = x/2, column = v/12;
  uint8_t bit = ((~x)&1) | (11 - v%12)<<1;
  uint32_t pixels = ReadBlock(row, column);
  return number(pixels>>bit & 1);
}

extern const uint8_t CharMap[96][6] PROGMEM;

object *fn_drawchar (object *args, object *env) {
  (void) env;
  uint8_t colour = 1, bg = 0, size = 1;
  int x0 = checkinteger(first(args)), y0 = checkinteger(second(args));
  char c = checkchar(third(args));
  object *more = cdr(cddr(args));
  if (more != NULL) {
    colour = checkinteger(car(more));
    more = cdr(more);
    if (more != NULL) {
      bg = checkinteger(car(more));
      more = cdr(more);
      if (more != NULL) size = checkinteger(car(more));
    }
  }
  for (int y=0; y<8; y++) {
    for (int x=0; x<6; x++) {
      uint8_t pixel = (pgm_read_byte(&CharMap[c-32][x])>>y & 1) ^ bg;
      if (size == 1) PlotPoint(x0+x, ymax-(y0+y), pixel);
      else { MoveTo(x0+x*size, ymax-(y0+y*size)); FillRect(size, size, pixel); }
    }
  }
  PlotPoint(255, 0, colour); // Flush
  return nil;
}

object *fn_setcursor (object *args, object *env) {
  (void) env, (void) args;
  return nil;
}

// Lisp Badge plotting

object *fn_plot (object *args, object *env) {
  int xaxis = -1, yaxis = -1;
  ClearDisplay(0); // Clear display
  if (args != NULL && integerp(first(args))) { xaxis = checkinteger(first(args)); args = cdr(args); }
  if (args != NULL && integerp(first(args))) { yaxis = checkinteger(first(args)); args = cdr(args); }
  int nargs = min(listlength(args), 4);
  for (int x=0; x<256; x++) {
    object *rest = args;
    for (int n=0; n<nargs; n++) {
      object *function = first(rest);
      int y = checkinteger(apply(function, cons(number(x), NULL), env));
      PlotPoint(x, ymax-y, 1);
      rest = cdr(rest);
    }
  }
  PlotPoint(255, 0, 1); // Flush
  if (yaxis != -1) { MoveTo(0, yaxis); DrawTo(249, yaxis, 1); }
  if (xaxis != -1) { MoveTo(xaxis, 0); DrawTo(xaxis, 121, 1); }
  return bsymbol(NOTHING);
}

object *fn_plot3d (object *args, object *env) {
  int xaxis = -1, yaxis = -1;
  ClearDisplay(0); // Clear display
  if (args != NULL && integerp(first(args))) { xaxis = checkinteger(first(args)); args = cdr(args); }
  if (args != NULL && integerp(first(args))) { yaxis = checkinteger(first(args)); args = cdr(args); }
  if (args != NULL) {
    object *function = first(args);
    for (uint8_t column=0; column<125; column++) {
      for (uint8_t line=0; line<12; line++) {
        uint32_t block = 0;
        for (uint8_t h=0; h<2; h++) {
          for (uint8_t v=0; v<12; v++) {
            int x = column*2 + h, y = line*12 + v, z;
            if (x == xaxis || y == yaxis) z = 1;
            else z = checkinteger(apply(function, cons(number(x), cons(number(y), NULL)), env));
            block = block | (uint32_t)z<<((11-v)*2 + (1-h));
          }
        }
        PlotBlock(block, column, line);
      }
    }
  }
  while (!tstflag(ESCAPE)); clrflag(ESCAPE);
  return bsymbol(NOTHING);
}

object *fn_glyphpixel (object *args, object *env) {
  (void) env;
  uint8_t c = 0, x = 6, y = 8;
  c = checkchar(first(args));
  x = checkinteger(second(args));
  y = checkinteger(third(args));
  if (x > 5 || y > 7) return number(0);
  return pgm_read_byte(&CharMap[(c & 0x7f) - 32][x]) & 1 << (7 - y) ? number(15) : number(0);
}

object *fn_checkkey (object *args, object *env) {
  (void) env;
  return checkkey(checkchar(first(args))) ? tee : nil;
}

object *fn_keyboard (object *args, object *env) {
  (void) env;
  object *enable = first(args);
  keyboard(enable != NULL);
  return enable;
}

// Error handling

object *sp_unwindprotect (object *args, object *env) {
  if (args == NULL) error2(toofewargs);
  object *current_GCStack = GCStack;
  jmp_buf dynamic_handler;
  jmp_buf *previous_handler = handler;
  handler = &dynamic_handler;
  object *protected_form = first(args);
  object *result;

  bool signaled = false;
  if (!setjmp(dynamic_handler)) {
    result = eval(protected_form, env);
  } else {
    GCStack = current_GCStack;
    signaled = true;
  }
  handler = previous_handler;

  object *protective_forms = cdr(args);
  while (protective_forms != NULL) {
    eval(car(protective_forms), env);
    if (tstflag(RETURNFLAG)) break;
    protective_forms = cdr(protective_forms);
  }

  if (!signaled) return result;
  GCStack = NULL;
  longjmp(*handler, 1);
}

object *sp_ignoreerrors (object *args, object *env) {
  object *current_GCStack = GCStack;
  jmp_buf dynamic_handler;
  jmp_buf *previous_handler = handler;
  handler = &dynamic_handler;
  object *result = nil;

  bool muffled = tstflag(MUFFLEERRORS);
  setflag(MUFFLEERRORS);
  bool signaled = false;
  if (!setjmp(dynamic_handler)) {
    while (args != NULL) {
      result = eval(car(args), env);
      if (tstflag(RETURNFLAG)) break;
      args = cdr(args);
    }
  } else {
    GCStack = current_GCStack;
    signaled = true;
  }
  handler = previous_handler;
  if (!muffled) clrflag(MUFFLEERRORS);

  if (signaled) return bsymbol(NOTHING);
  else return result;
}

object *sp_error (object *args, object *env) {
  object *message = eval(cons(bsymbol(FORMAT), cons(nil, args)), env);
  if (!tstflag(MUFFLEERRORS)) {
    char temp = Flags;
    clrflag(PRINTREADABLY);
    pfstring(PSTR("Error: "), pserial); printstring(message, pserial);
    Flags = temp;
    pln(pserial);
  }
  GCStack = NULL;
  longjmp(*handler, 1);
}

// Built-in symbol names
const char string0[] PROGMEM = "nil";
const char string1[] PROGMEM = "t";
const char string2[] PROGMEM = "nothing";
const char string3[] PROGMEM = "&optional";
const char string4[] PROGMEM = "*features*";
const char string5[] PROGMEM = ":initial-element";
const char string6[] PROGMEM = ":element-type";
const char string7[] PROGMEM = ":test";
const char string8[] PROGMEM = "bit";
const char string9[] PROGMEM = "&rest";
const char string10[] PROGMEM = "lambda";
const char string11[] PROGMEM = "let";
const char string12[] PROGMEM = "let*";
const char string13[] PROGMEM = "closure";
const char string14[] PROGMEM = "*p*";
const char string15[] PROGMEM = "quote";
const char string16[] PROGMEM = "defun";
const char string17[] PROGMEM = "defvar";
const char string18[] PROGMEM = "defcode";
const char string19[] PROGMEM = "eq";
const char string20[] PROGMEM = "car";
const char string21[] PROGMEM = "first";
const char string22[] PROGMEM = "cdr";
const char string23[] PROGMEM = "rest";
const char string24[] PROGMEM = "nth";
const char string25[] PROGMEM = "aref";
const char string26[] PROGMEM = "char";
const char string27[] PROGMEM = "string";
const char string28[] PROGMEM = "pinmode";
const char string29[] PROGMEM = "digitalwrite";
const char string30[] PROGMEM = "analogread";
const char string31[] PROGMEM = "analogreference";
const char string32[] PROGMEM = "register";
const char string33[] PROGMEM = "format";
const char string34[] PROGMEM = "or";
const char string35[] PROGMEM = "setq";
const char string36[] PROGMEM = "loop";
const char string37[] PROGMEM = "push";
const char string38[] PROGMEM = "pop";
const char string39[] PROGMEM = "incf";
const char string40[] PROGMEM = "decf";
const char string41[] PROGMEM = "setf";
const char string42[] PROGMEM = "dolist";
const char string43[] PROGMEM = "dotimes";
const char string44[] PROGMEM = "do";
const char string45[] PROGMEM = "do*";
const char string46[] PROGMEM = "trace";
const char string47[] PROGMEM = "untrace";
const char string48[] PROGMEM = "for-millis";
const char string49[] PROGMEM = "time";
const char string50[] PROGMEM = "with-output-to-string";
const char string51[] PROGMEM = "with-serial";
const char string52[] PROGMEM = "with-i2c";
const char string53[] PROGMEM = "with-spi";
const char string54[] PROGMEM = "with-sd-card";
const char string55[] PROGMEM = "progn";
const char string56[] PROGMEM = "if";
const char string57[] PROGMEM = "cond";
const char string58[] PROGMEM = "when";
const char string59[] PROGMEM = "unless";
const char string60[] PROGMEM = "case";
const char string61[] PROGMEM = "and";
const char string62[] PROGMEM = "not";
const char string63[] PROGMEM = "null";
const char string64[] PROGMEM = "cons";
const char string65[] PROGMEM = "atom";
const char string66[] PROGMEM = "listp";
const char string67[] PROGMEM = "consp";
const char string68[] PROGMEM = "symbolp";
const char string69[] PROGMEM = "arrayp";
const char string70[] PROGMEM = "boundp";
const char string71[] PROGMEM = "keywordp";
const char string72[] PROGMEM = "set";
const char string73[] PROGMEM = "streamp";
const char string74[] PROGMEM = "equal";
const char string75[] PROGMEM = "caar";
const char string76[] PROGMEM = "cadr";
const char string77[] PROGMEM = "second";
const char string78[] PROGMEM = "cdar";
const char string79[] PROGMEM = "cddr";
const char string80[] PROGMEM = "caaar";
const char string81[] PROGMEM = "caadr";
const char string82[] PROGMEM = "cadar";
const char string83[] PROGMEM = "caddr";
const char string84[] PROGMEM = "third";
const char string85[] PROGMEM = "cdaar";
const char string86[] PROGMEM = "cdadr";
const char string87[] PROGMEM = "cddar";
const char string88[] PROGMEM = "cdddr";
const char string89[] PROGMEM = "length";
const char string90[] PROGMEM = "array-dimensions";
const char string91[] PROGMEM = "list";
const char string92[] PROGMEM = "copy-list";
const char string93[] PROGMEM = "make-array";
const char string94[] PROGMEM = "reverse";
const char string95[] PROGMEM = "assoc";
const char string96[] PROGMEM = "member";
const char string97[] PROGMEM = "apply";
const char string98[] PROGMEM = "funcall";
const char string99[] PROGMEM = "append";
const char string100[] PROGMEM = "mapc";
const char string101[] PROGMEM = "mapl";
const char string102[] PROGMEM = "mapcar";
const char string103[] PROGMEM = "mapcan";
const char string104[] PROGMEM = "maplist";
const char string105[] PROGMEM = "mapcon";
const char string106[] PROGMEM = "+";
const char string107[] PROGMEM = "-";
const char string108[] PROGMEM = "*";
const char string109[] PROGMEM = "/";
const char string110[] PROGMEM = "truncate";
const char string111[] PROGMEM = "mod";
const char string112[] PROGMEM = "1+";
const char string113[] PROGMEM = "1-";
const char string114[] PROGMEM = "abs";
const char string115[] PROGMEM = "random";
const char string116[] PROGMEM = "max";
const char string117[] PROGMEM = "min";
const char string118[] PROGMEM = "/=";
const char string119[] PROGMEM = "=";
const char string120[] PROGMEM = "<";
const char string121[] PROGMEM = "<=";
const char string122[] PROGMEM = ">";
const char string123[] PROGMEM = ">=";
const char string124[] PROGMEM = "plusp";
const char string125[] PROGMEM = "minusp";
const char string126[] PROGMEM = "zerop";
const char string127[] PROGMEM = "oddp";
const char string128[] PROGMEM = "evenp";
const char string129[] PROGMEM = "integerp";
const char string130[] PROGMEM = "numberp";
const char string131[] PROGMEM = "char-code";
const char string132[] PROGMEM = "code-char";
const char string133[] PROGMEM = "characterp";
const char string134[] PROGMEM = "stringp";
const char string135[] PROGMEM = "string=";
const char string136[] PROGMEM = "string<";
const char string137[] PROGMEM = "string>";
const char string138[] PROGMEM = "string/=";
const char string139[] PROGMEM = "string<=";
const char string140[] PROGMEM = "string>=";
const char string141[] PROGMEM = "sort";
const char string142[] PROGMEM = "concatenate";
const char string143[] PROGMEM = "subseq";
const char string144[] PROGMEM = "search";
const char string145[] PROGMEM = "read-from-string";
const char string146[] PROGMEM = "princ-to-string";
const char string147[] PROGMEM = "prin1-to-string";
const char string148[] PROGMEM = "logand";
const char string149[] PROGMEM = "logior";
const char string150[] PROGMEM = "logxor";
const char string151[] PROGMEM = "lognot";
const char string152[] PROGMEM = "ash";
const char string153[] PROGMEM = "logbitp";
const char string154[] PROGMEM = "eval";
const char string155[] PROGMEM = "return";
const char string156[] PROGMEM = "globals";
const char string157[] PROGMEM = "locals";
const char string158[] PROGMEM = "makunbound";
const char string159[] PROGMEM = "break";
const char string160[] PROGMEM = "read";
const char string161[] PROGMEM = "prin1";
const char string162[] PROGMEM = "print";
const char string163[] PROGMEM = "princ";
const char string164[] PROGMEM = "terpri";
const char string165[] PROGMEM = "read-byte";
const char string166[] PROGMEM = "read-line";
const char string167[] PROGMEM = "write-byte";
const char string168[] PROGMEM = "write-string";
const char string169[] PROGMEM = "write-line";
const char string170[] PROGMEM = "restart-i2c";
const char string171[] PROGMEM = "gc";
const char string172[] PROGMEM = "room";
const char string173[] PROGMEM = "save-image";
const char string174[] PROGMEM = "load-image";
const char string175[] PROGMEM = "cls";
const char string176[] PROGMEM = "digitalread";
const char string177[] PROGMEM = "analogreadresolution";
const char string178[] PROGMEM = "analogwrite";
const char string179[] PROGMEM = "dacreference";
const char string180[] PROGMEM = "delay";
const char string181[] PROGMEM = "millis";
const char string182[] PROGMEM = "sleep";
const char string183[] PROGMEM = "note";
const char string184[] PROGMEM = "edit";
const char string185[] PROGMEM = "pprint";
const char string186[] PROGMEM = "pprintall";
const char string187[] PROGMEM = "require";
const char string188[] PROGMEM = "list-library";
const char string189[] PROGMEM = "?";
const char string190[] PROGMEM = "documentation";
const char string191[] PROGMEM = "apropos";
const char string192[] PROGMEM = "apropos-list";
const char string193[] PROGMEM = "unwind-protect";
const char string194[] PROGMEM = "ignore-errors";
const char string195[] PROGMEM = "error";
const char stringG1[] PROGMEM = "draw-pixel";
const char stringG2[] PROGMEM = "draw-line";
const char stringG3[] PROGMEM = "draw-rect";
const char stringG4[] PROGMEM = "fill-rect";
const char stringG5[] PROGMEM = "draw-circle";
const char stringG6[] PROGMEM = "fill-circle";
const char stringG7[] PROGMEM = "draw-triangle";
const char stringG8[] PROGMEM = "fill-triangle";
const char stringG9[] PROGMEM = "glyph-pixel";
const char stringG10[] PROGMEM = "plot-pixel";
const char stringG11[] PROGMEM = "fill-screen";
const char stringG12[] PROGMEM = "get-pixel";
const char stringG13[] PROGMEM = "draw-char";
const char stringG14[] PROGMEM = "set-cursor";
const char stringG15[] PROGMEM = "plot";
const char stringG16[] PROGMEM = "plot3d";
const char stringG17[] PROGMEM = "check-key";
const char stringG18[] PROGMEM = "keyboard";
const char stringG19[] PROGMEM = ":shift-key";
const char stringG20[] PROGMEM = ":meta-key";
const char string196[] PROGMEM = ":led-builtin";
const char string197[] PROGMEM = ":high";
const char string198[] PROGMEM = ":low";
const char string199[] PROGMEM = ":input";
const char string200[] PROGMEM = ":input-pullup";
const char string201[] PROGMEM = ":output";
const char string202[] PROGMEM = ":default";
const char string203[] PROGMEM = ":vdd";
const char string204[] PROGMEM = ":internal1v024";
const char string205[] PROGMEM = ":internal2v048";
const char string206[] PROGMEM = ":internal4v096";
const char string207[] PROGMEM = ":internal2v5";
const char string208[] PROGMEM = ":external";
const char string209[] PROGMEM = ":adc-dac0";
const char string210[] PROGMEM = ":adc-temperature";
const char string211[] PROGMEM = ":porta-dir";
const char string212[] PROGMEM = ":porta-out";
const char string213[] PROGMEM = ":porta-in";
const char string214[] PROGMEM = ":portb-dir";
const char string215[] PROGMEM = ":portb-out";
const char string216[] PROGMEM = ":portb-in";
const char string217[] PROGMEM = ":portc-dir";
const char string218[] PROGMEM = ":portc-out";
const char string219[] PROGMEM = ":portc-in";
const char string220[] PROGMEM = ":portd-dir";
const char string221[] PROGMEM = ":portd-out";
const char string222[] PROGMEM = ":portd-in";
const char string223[] PROGMEM = ":porte-dir";
const char string224[] PROGMEM = ":porte-out";
const char string225[] PROGMEM = ":porte-in";
const char string226[] PROGMEM = ":portf-dir";
const char string227[] PROGMEM = ":portf-out";
const char string228[] PROGMEM = ":portf-in";

// Documentation strings
// Documentation strings
const char doc0[] PROGMEM = "nil\n"
"A symbol equivalent to the empty list (). Also represents false.";
const char doc1[] PROGMEM = "t\n"
"A symbol representing true.";
const char doc2[] PROGMEM = "nothing\n"
"A symbol with no value.\n"
"It is useful if you want to suppress printing the result of evaluating a function.";
const char doc3[] PROGMEM = "&optional\n"
"Can be followed by one or more optional parameters in a lambda or defun parameter list.";
const char doc4[] PROGMEM = "*features*\n"
"Returns a list of keywords representing features supported by this platform.";
const char doc9[] PROGMEM = "&rest\n"
"Can be followed by a parameter in a lambda or defun parameter list,\n"
"and is assigned a list of the corresponding arguments.";
const char doc10[] PROGMEM = "(lambda (parameter*) form*)\n"
"Creates an unnamed function with parameters. The body is evaluated with the parameters as local variables\n"
"whose initial values are defined by the values of the forms after the lambda form.";
const char doc11[] PROGMEM = "(let ((var value) ... ) forms*)\n"
"Declares local variables with values, and evaluates the forms with those local variables.";
const char doc12[] PROGMEM = "(let* ((var value) ... ) forms*)\n"
"Declares local variables with values, and evaluates the forms with those local variables.\n"
"Each declaration can refer to local variables that have been defined earlier in the let*.";
const char doc16[] PROGMEM = "(defun name (parameters) form*)\n"
"Defines a function.";
const char doc17[] PROGMEM = "(defvar variable form)\n"
"Defines a global variable.";
const char doc18[] PROGMEM = "(defcode name (parameters) form*)\n"
"Creates a machine-code function called name from a series of 16-bit integers given in the body of the form.\n"
"These are written into RAM, and can be executed by calling the function in the same way as a normal Lisp function.";
const char doc19[] PROGMEM = "(eq item item)\n"
"Tests whether the two arguments are the same symbol, same character, equal numbers,\n"
"or point to the same cons, and returns t or nil as appropriate.";
const char doc20[] PROGMEM = "(car list)\n"
"Returns the first item in a list.";
const char doc22[] PROGMEM = "(cdr list)\n"
"Returns a list with the first item removed.";
const char doc24[] PROGMEM = "(nth number list)\n"
"Returns the nth item in list, counting from zero.";
const char doc25[] PROGMEM = "(aref array index [index*])\n"
"Returns an element from the specified array.";
const char doc26[] PROGMEM = "(char string n)\n"
"Returns the nth character in a string, counting from zero.";
const char doc27[] PROGMEM = "(string item)\n"
"Converts its argument to a string.";
const char doc28[] PROGMEM = "(pinmode pin mode)\n"
"Sets the input/output mode of an Arduino pin number, and returns nil.\n"
"The mode parameter can be an integer, a keyword, or t or nil.";
const char doc29[] PROGMEM = "(digitalwrite pin state)\n"
"Sets the state of the specified Arduino pin number.";
const char doc30[] PROGMEM = "(analogread pin)\n"
"Reads the specified Arduino analogue pin number and returns the value.";
const char doc31[] PROGMEM = "(analogreference keyword)\n"
"Specifies a keyword to set the analogue reference voltage used for analogue input.";
const char doc32[] PROGMEM = "(register address [value])\n"
"Reads or writes the value of a peripheral register.\n"
"If value is not specified the function returns the value of the register at address.\n"
"If value is specified the value is written to the register at address and the function returns value.";
const char doc33[] PROGMEM = "(format output controlstring [arguments]*)\n"
"Outputs its arguments formatted according to the format directives in controlstring.";
const char doc34[] PROGMEM = "(or item*)\n"
"Evaluates its arguments until one returns non-nil, and returns its value.";
const char doc35[] PROGMEM = "(setq symbol value [symbol value]*)\n"
"For each pair of arguments assigns the value of the second argument\n"
"to the variable specified in the first argument.";
const char doc36[] PROGMEM = "(loop forms*)\n"
"Executes its arguments repeatedly until one of the arguments calls (return),\n"
"which then causes an exit from the loop.";
const char doc37[] PROGMEM = "(push item place)\n"
"Modifies the value of place, which should be a list, to add item onto the front of the list,\n"
"and returns the new list.";
const char doc38[] PROGMEM = "(pop place)\n"
"Modifies the value of place, which should be a non-nil list, to remove its first item,\n"
"and returns that item.";
const char doc39[] PROGMEM = "(incf place [number])\n"
"Increments a place, which should have an numeric value, and returns the result.\n"
"The third argument is an optional increment which defaults to 1.";
const char doc40[] PROGMEM = "(decf place [number])\n"
"Decrements a place, which should have an numeric value, and returns the result.\n"
"The third argument is an optional decrement which defaults to 1.";
const char doc41[] PROGMEM = "(setf place value [place value]*)\n"
"For each pair of arguments modifies a place to the result of evaluating value.";
const char doc42[] PROGMEM = "(dolist (var list [result]) form*)\n"
"Sets the local variable var to each element of list in turn, and executes the forms.\n"
"It then returns result, or nil if result is omitted.";
const char doc43[] PROGMEM = "(dotimes (var number [result]) form*)\n"
"Executes the forms number times, with the local variable var set to each integer from 0 to number-1 in turn.\n"
"It then returns result, or nil if result is omitted.";
const char doc44[] PROGMEM = "(do ((var [init [step]])*) (end-test result*) form*)\n"
"Accepts an arbitrary number of iteration vars, which are initialised to init and stepped by step sequentially.\n"
"The forms are executed until end-test is true. It returns result.";
const char doc45[] PROGMEM = "(do* ((var [init [step]])*) (end-test result*) form*)\n"
"Accepts an arbitrary number of iteration vars, which are initialised to init and stepped by step in parallel.\n"
"The forms are executed until end-test is true. It returns result.";
const char doc46[] PROGMEM = "(trace [function]*)\n"
"Turns on tracing of up to TRACEMAX user-defined functions,\n"
"and returns a list of the functions currently being traced.";
const char doc47[] PROGMEM = "(untrace [function]*)\n"
"Turns off tracing of up to TRACEMAX user-defined functions, and returns a list of the functions untraced.\n"
"If no functions are specified it untraces all functions.";
const char doc48[] PROGMEM = "(for-millis ([number]) form*)\n"
"Executes the forms and then waits until a total of number milliseconds have elapsed.\n"
"Returns the total number of milliseconds taken.";
const char doc49[] PROGMEM = "(time form)\n"
"Prints the value returned by the form, and the time taken to evaluate the form\n"
"in milliseconds or seconds.";
const char doc50[] PROGMEM = "(with-output-to-string (str) form*)\n"
"Returns a string containing the output to the stream variable str.";
const char doc51[] PROGMEM = "(with-serial (str port [baud]) form*)\n"
"Evaluates the forms with str bound to a serial-stream using port.\n"
"The optional baud gives the baud rate divided by 100, default 96.";
const char doc52[] PROGMEM = "(with-i2c (str [port] address [read-p]) form*)\n"
"Evaluates the forms with str bound to an i2c-stream defined by address.\n"
"If read-p is nil or omitted the stream is written to, otherwise it specifies the number of bytes\n"
"to be read from the stream. The port if specified is ignored.";
const char doc53[] PROGMEM = "(with-spi (str pin [clock] [bitorder] [mode]) form*)\n"
"Evaluates the forms with str bound to an spi-stream.\n"
"The parameters specify the enable pin, clock in kHz (default 4000),\n"
"bitorder 0 for LSBFIRST and 1 for MSBFIRST (default 1), and SPI mode (default 0).";
const char doc54[] PROGMEM = "(with-sd-card (str filename [mode]) form*)\n"
"Evaluates the forms with str bound to an sd-stream reading from or writing to the file filename.\n"
"If mode is omitted the file is read, otherwise 0 means read, 1 write-append, or 2 write-overwrite.";
const char doc55[] PROGMEM = "(progn form*)\n"
"Evaluates several forms grouped together into a block, and returns the result of evaluating the last form.";
const char doc56[] PROGMEM = "(if test then [else])\n"
"Evaluates test. If it's non-nil the form then is evaluated and returned;\n"
"otherwise the form else is evaluated and returned.";
const char doc57[] PROGMEM = "(cond ((test form*) (test form*) ... ))\n"
"Each argument is a list consisting of a test optionally followed by one or more forms.\n"
"If the test evaluates to non-nil the forms are evaluated, and the last value is returned as the result of the cond.\n"
"If the test evaluates to nil, none of the forms are evaluated, and the next argument is processed in the same way.";
const char doc58[] PROGMEM = "(when test form*)\n"
"Evaluates the test. If it's non-nil the forms are evaluated and the last value is returned.";
const char doc59[] PROGMEM = "(unless test form*)\n"
"Evaluates the test. If it's nil the forms are evaluated and the last value is returned.";
const char doc60[] PROGMEM = "(case keyform ((key form*) (key form*) ... ))\n"
"Evaluates a keyform to produce a test key, and then tests this against a series of arguments,\n"
"each of which is a list containing a key optionally followed by one or more forms.";
const char doc61[] PROGMEM = "(and item*)\n"
"Evaluates its arguments until one returns nil, and returns the last value.";
const char doc62[] PROGMEM = "(not item)\n"
"Returns t if its argument is nil, or nil otherwise. Equivalent to null.";
const char doc64[] PROGMEM = "(cons item item)\n"
"If the second argument is a list, cons returns a new list with item added to the front of the list.\n"
"If the second argument isn't a list cons returns a dotted pair.";
const char doc65[] PROGMEM = "(atom item)\n"
"Returns t if its argument is a single number, symbol, or nil.";
const char doc66[] PROGMEM = "(listp item)\n"
"Returns t if its argument is a list.";
const char doc67[] PROGMEM = "(consp item)\n"
"Returns t if its argument is a non-null list.";
const char doc68[] PROGMEM = "(symbolp item)\n"
"Returns t if its argument is a symbol.";
const char doc69[] PROGMEM = "(arrayp item)\n"
"Returns t if its argument is an array.";
const char doc70[] PROGMEM = "(boundp item)\n"
"Returns t if its argument is a symbol with a value.";
const char doc71[] PROGMEM = "(keywordp item)\n"
"Returns t if its argument is a built-in or user-defined keyword.";
const char doc72[] PROGMEM = "(set symbol value [symbol value]*)\n"
"For each pair of arguments, assigns the value of the second argument to the value of the first argument.";
const char doc73[] PROGMEM = "(streamp item)\n"
"Returns t if its argument is a stream.";
const char doc74[] PROGMEM = "(equal item item)\n"
"Tests whether the two arguments are the same symbol, same character, equal numbers,\n"
"or point to the same cons, and returns t or nil as appropriate.";
const char doc75[] PROGMEM = "(caar list)";
const char doc76[] PROGMEM = "(cadr list)";
const char doc78[] PROGMEM = "(cdar list)\n"
"Equivalent to (cdr (car list)).";
const char doc79[] PROGMEM = "(cddr list)\n"
"Equivalent to (cdr (cdr list)).";
const char doc80[] PROGMEM = "(caaar list)\n"
"Equivalent to (car (car (car list))).";
const char doc81[] PROGMEM = "(caadr list)\n"
"Equivalent to (car (car (cdar list))).";
const char doc82[] PROGMEM = "(cadar list)\n"
"Equivalent to (car (cdr (car list))).";
const char doc83[] PROGMEM = "(caddr list)\n"
"Equivalent to (car (cdr (cdr list))).";
const char doc85[] PROGMEM = "(cdaar list)\n"
"Equivalent to (cdar (car (car list))).";
const char doc86[] PROGMEM = "(cdadr list)\n"
"Equivalent to (cdr (car (cdr list))).";
const char doc87[] PROGMEM = "(cddar list)\n"
"Equivalent to (cdr (cdr (car list))).";
const char doc88[] PROGMEM = "(cdddr list)\n"
"Equivalent to (cdr (cdr (cdr list))).";
const char doc89[] PROGMEM = "(length item)\n"
"Returns the number of items in a list, the length of a string, or the length of a one-dimensional array.";
const char doc90[] PROGMEM = "(array-dimensions item)\n"
"Returns a list of the dimensions of an array.";
const char doc91[] PROGMEM = "(list item*)\n"
"Returns a list of the values of its arguments.";
const char doc92[] PROGMEM = "(copy-list list)\n"
"Returns a copy of a list.";
const char doc93[] PROGMEM = "(make-array size [:initial-element element] [:element-type 'bit])\n"
"If size is an integer it creates a one-dimensional array with elements from 0 to size-1.\n"
"If size is a list of n integers it creates an n-dimensional array with those dimensions.\n"
"If :element-type 'bit is specified the array is a bit array.";
const char doc94[] PROGMEM = "(reverse list)\n"
"Returns a list with the elements of list in reverse order.";
const char doc95[] PROGMEM = "(assoc key list [:test function])\n"
"Looks up a key in an association list of (key . value) pairs, using eq or the specified test function,\n"
"and returns the matching pair, or nil if no pair is found.";
const char doc96[] PROGMEM = "(member item list [:test function])\n"
"Searches for an item in a list, using eq or the specified test function, and returns the list starting\n"
"from the first occurrence of the item, or nil if it is not found.";
const char doc97[] PROGMEM = "(apply function list)\n"
"Returns the result of evaluating function, with the list of arguments specified by the second parameter.";
const char doc98[] PROGMEM = "(funcall function argument*)\n"
"Evaluates function with the specified arguments.";
const char doc99[] PROGMEM = "(append list*)\n"
"Joins its arguments, which should be lists, into a single list.";
const char doc100[] PROGMEM = "(mapc function list1 [list]*)\n"
"Applies the function to each element in one or more lists, ignoring the results.\n"
"It returns the first list argument.";
const char doc101[] PROGMEM = "(mapl function list1 [list]*)\n"
"Applies the function to one or more lists and then successive cdrs of those lists,\n"
"ignoring the results. It returns the first list argument.";
const char doc102[] PROGMEM = "(mapcar function list1 [list]*)\n"
"Applies the function to each element in one or more lists, and returns the resulting list.";
const char doc103[] PROGMEM = "(mapcan function list1 [list]*)\n"
"Applies the function to each element in one or more lists. The results should be lists,\n"
"and these are destructively concatenated together to give the value returned.";
const char doc104[] PROGMEM = "(maplist function list1 [list]*)\n"
"Applies the function to one or more lists and then successive cdrs of those lists,\n"
"and returns the resulting list.";
const char doc105[] PROGMEM = "(mapcon function list1 [list]*)\n"
"Applies the function to one or more lists and then successive cdrs of those lists,\n"
"and these are destructively concatenated together to give the value returned.";
const char doc106[] PROGMEM = "(+ number*)\n"
"Adds its arguments together.";
const char doc107[] PROGMEM = "(- number*)\n"
"If there is one argument, negates the argument.\n"
"If there are two or more arguments, subtracts the second and subsequent arguments from the first argument.";
const char doc108[] PROGMEM = "(* number*)\n"
"Multiplies its arguments together.";
const char doc109[] PROGMEM = "(/ number*)\n"
"Divides the first argument by the second and subsequent arguments.";
const char doc111[] PROGMEM = "(mod number number)\n"
"Returns its first argument modulo the second argument.";
const char doc112[] PROGMEM = "(1+ number)\n"
"Adds one to its argument and returns it.";
const char doc113[] PROGMEM = "(1- number)\n"
"Subtracts one from its argument and returns it.";
const char doc114[] PROGMEM = "(abs number)\n"
"Returns the absolute, positive value of its argument.";
const char doc115[] PROGMEM = "(random number)\n"
"Returns a random number between 0 and one less than its argument.";
const char doc116[] PROGMEM = "(max number*)\n"
"Returns the maximum of one or more arguments.";
const char doc117[] PROGMEM = "(min number*)\n"
"Returns the minimum of one or more arguments.";
const char doc118[] PROGMEM = "(/= number*)\n"
"Returns t if none of the arguments are equal, or nil if two or more arguments are equal.";
const char doc119[] PROGMEM = "(= number*)\n"
"Returns t if all the arguments, which must be numbers, are numerically equal, and nil otherwise.";
const char doc120[] PROGMEM = "(< number*)\n"
"Returns t if each argument is less than the next argument, and nil otherwise.";
const char doc121[] PROGMEM = "(<= number*)\n"
"Returns t if each argument is less than or equal to the next argument, and nil otherwise.";
const char doc122[] PROGMEM = "(> number*)\n"
"Returns t if each argument is greater than the next argument, and nil otherwise.";
const char doc123[] PROGMEM = "(>= number*)\n"
"Returns t if each argument is greater than or equal to the next argument, and nil otherwise.";
const char doc124[] PROGMEM = "(plusp number)\n"
"Returns t if the argument is greater than zero, or nil otherwise.";
const char doc126[] PROGMEM = "(zerop number)\n"
"Returns t if the argument is zero.";
const char doc127[] PROGMEM = "(oddp number)\n"
"Returns t if the integer argument is odd.";
const char doc128[] PROGMEM = "(evenp number)\n"
"Returns t if the integer argument is even.";
const char doc129[] PROGMEM = "(integerp number)\n"
"Returns t if the argument is an integer.";
const char doc131[] PROGMEM = "(char-code character)\n"
"Returns the ASCII code for a character, as an integer.";
const char doc132[] PROGMEM = "(code-char integer)\n"
"Returns the character for the specified ASCII code.";
const char doc133[] PROGMEM = "(characterp item)\n"
"Returns t if the argument is a character and nil otherwise.";
const char doc134[] PROGMEM = "(stringp item)\n"
"Returns t if the argument is a string and nil otherwise.";
const char doc135[] PROGMEM = "(string= string string)\n"
"Returns t if the two strings are the same, or nil otherwise.";
const char doc136[] PROGMEM = "(string< string string)\n"
"Returns the index to the first mismatch if the first string is alphabetically less than the second string,\n"
"or nil otherwise.";
const char doc137[] PROGMEM = "(string> string string)\n"
"Returns the index to the first mismatch if the first string is alphabetically greater than the second string,\n"
"or nil otherwise.";
const char doc138[] PROGMEM = "(string/= string string)\n"
"Returns the index to the first mismatch if the two strings are not the same, or nil otherwise.";
const char doc139[] PROGMEM = "(string<= string string)\n"
"Returns the index to the first mismatch if the first string is alphabetically less than or equal to\n"
"the second string, or nil otherwise.";
const char doc140[] PROGMEM = "(string>= string string)\n"
"Returns the index to the first mismatch if the first string is alphabetically greater than or equal to\n"
"the second string, or nil otherwise.";
const char doc141[] PROGMEM = "(sort list test)\n"
"Destructively sorts list according to the test function, using an insertion sort, and returns the sorted list.";
const char doc142[] PROGMEM = "(concatenate 'string string*)\n"
"Joins together the strings given in the second and subsequent arguments, and returns a single string.";
const char doc143[] PROGMEM = "(subseq seq start [end])\n"
"Returns a subsequence of a list or string from item start to item end-1.";
const char doc144[] PROGMEM = "(search pattern target [:test function])\n"
"Returns the index of the first occurrence of pattern in target, or nil if it's not found.\n"
"The target can be a list or string. If it's a list a test function can be specified; default eq.";
const char doc145[] PROGMEM = "(read-from-string string)\n"
"Reads an atom or list from the specified string and returns it.";
const char doc146[] PROGMEM = "(princ-to-string item)\n"
"Prints its argument to a string, and returns the string.\n"
"Characters and strings are printed without quotation marks or escape characters.";
const char doc147[] PROGMEM = "(prin1-to-string item [stream])\n"
"Prints its argument to a string, and returns the string.\n"
"Characters and strings are printed with quotation marks and escape characters,\n"
"in a format that will be suitable for read-from-string.";
const char doc148[] PROGMEM = "(logand [value*])\n"
"Returns the bitwise & of the values.";
const char doc149[] PROGMEM = "(logior [value*])\n"
"Returns the bitwise | of the values.";
const char doc150[] PROGMEM = "(logxor [value*])\n"
"Returns the bitwise ^ of the values.";
const char doc151[] PROGMEM = "(lognot value)\n"
"Returns the bitwise logical NOT of the value.";
const char doc152[] PROGMEM = "(ash value shift)\n"
"Returns the result of bitwise shifting value by shift bits. If shift is positive, value is shifted to the left.";
const char doc153[] PROGMEM = "(logbitp bit value)\n"
"Returns t if bit number bit in value is a '1', and nil if it is a '0'.";
const char doc154[] PROGMEM = "(eval form*)\n"
"Evaluates its argument an extra time.";
const char doc155[] PROGMEM = "(return [value])\n"
"Exits from a (dotimes ...), (dolist ...), or (loop ...) loop construct and returns value.";
const char doc156[] PROGMEM = "(globals)\n"
"Returns a list of global variables.";
const char doc157[] PROGMEM = "(locals)\n"
"Returns an association list of local variables and their values.";
const char doc158[] PROGMEM = "(makunbound symbol)\n"
"Removes the value of the symbol from GlobalEnv and returns the symbol.";
const char doc159[] PROGMEM = "(break)\n"
"Inserts a breakpoint in the program. When evaluated prints Break! and reenters the REPL.";
const char doc160[] PROGMEM = "(read [stream])\n"
"Reads an atom or list from the serial input and returns it.\n"
"If stream is specified the item is read from the specified stream.";
const char doc161[] PROGMEM = "(prin1 item [stream])\n"
"Prints its argument, and returns its value.\n"
"Strings are printed with quotation marks and escape characters.";
const char doc162[] PROGMEM = "(print item [stream])\n"
"Prints its argument with quotation marks and escape characters, on a new line, and followed by a space.\n"
"If stream is specified the argument is printed to the specified stream.";
const char doc163[] PROGMEM = "(princ item [stream])\n"
"Prints its argument, and returns its value.\n"
"Characters and strings are printed without quotation marks or escape characters.";
const char doc164[] PROGMEM = "(terpri [stream])\n"
"Prints a new line, and returns nil.\n"
"If stream is specified the new line is written to the specified stream.";
const char doc165[] PROGMEM = "(read-byte stream)\n"
"Reads a byte from a stream and returns it.";
const char doc166[] PROGMEM = "(read-line [stream])\n"
"Reads characters from the serial input up to a newline character, and returns them as a string, excluding the newline.\n"
"If stream is specified the line is read from the specified stream.";
const char doc167[] PROGMEM = "(write-byte number [stream])\n"
"Writes a byte to a stream.";
const char doc168[] PROGMEM = "(write-string string [stream])\n"
"Writes a string. If stream is specified the string is written to the stream.";
const char doc169[] PROGMEM = "(write-line string [stream])\n"
"Writes a string terminated by a newline character. If stream is specified the string is written to the stream.";
const char doc170[] PROGMEM = "(restart-i2c stream [read-p])\n"
"Restarts an i2c-stream.\n"
"If read-p is nil or omitted the stream is written to.\n"
"If read-p is an integer it specifies the number of bytes to be read from the stream.";
const char doc171[] PROGMEM = "(gc)\n"
"Forces a garbage collection and prints the number of objects collected, and the time taken.";
const char doc172[] PROGMEM = "(room)\n"
"Returns the number of free Lisp cells remaining.";
const char doc173[] PROGMEM = "(save-image [symbol])\n"
"Saves the current uLisp image to non-volatile memory or SD card so it can be loaded using load-image.";
const char doc174[] PROGMEM = "(load-image [filename])\n"
"Loads a saved uLisp image from non-volatile memory or SD card.";
const char doc175[] PROGMEM = "(cls)\n"
"Prints a clear-screen character.";
const char doc176[] PROGMEM = "(digitalread pin)\n"
"Reads the state of the specified Arduino pin number and returns t (high) or nil (low).";
const char doc177[] PROGMEM = "(analogreadresolution bits)\n"
"Specifies the resolution for the analogue inputs on platforms that support it.\n"
"The default resolution on all platforms is 10 bits.";
const char doc178[] PROGMEM = "(analogwrite pin value)\n"
"Writes the value to the specified Arduino pin number.";
const char doc179[] PROGMEM = "(dacreference value)\n"
"Sets the DAC voltage reference. AVR128DX48 only.";
const char doc180[] PROGMEM = "(delay number)\n"
"Delays for a specified number of milliseconds.";
const char doc181[] PROGMEM = "(millis)\n"
"Returns the time in milliseconds that uLisp has been running.";
const char doc182[] PROGMEM = "(sleep secs)\n"
"Puts the processor into a low-power sleep mode for secs.\n"
"Only supported on some platforms. On other platforms it does delay(1000*secs).";
const char doc183[] PROGMEM = "(note [pin] [note] [octave])\n"
"Generates a square wave on pin.\n"
"note represents the note in the well-tempered scale.\n"
"The argument octave can specify an octave; default 0.";
const char doc184[] PROGMEM = "(edit 'function)\n"
"Calls the Lisp tree editor to allow you to edit a function definition.";
const char doc185[] PROGMEM = "(pprint item [str])\n"
"Prints its argument, using the pretty printer, to display it formatted in a structured way.\n"
"If str is specified it prints to the specified stream. It returns no value.";
const char doc186[] PROGMEM = "(pprintall [str])\n"
"Pretty-prints the definition of every function and variable defined in the uLisp workspace.\n"
"If str is specified it prints to the specified stream. It returns no value.";
const char doc187[] PROGMEM = "(require 'symbol)\n"
"Loads the definition of a function defined with defun, or a variable defined with defvar, from the Lisp Library.\n"
"It returns t if it was loaded, or nil if the symbol is already defined or isn't defined in the Lisp Library.";
const char doc188[] PROGMEM = "(list-library)\n"
"Prints a list of the functions defined in the List Library.";
const char doc189[] PROGMEM = "(? item)\n"
"Prints the documentation string of a built-in or user-defined function.";
const char doc190[] PROGMEM = "(documentation 'symbol [type])\n"
"Returns the documentation string of a built-in or user-defined function. The type argument is ignored.";
const char doc191[] PROGMEM = "(apropos item)\n"
"Prints the user-defined and built-in functions whose names contain the specified string or symbol.";
const char doc192[] PROGMEM = "(apropos-list item)\n"
"Returns a list of user-defined and built-in functions whose names contain the specified string or symbol.";
const char doc193[] PROGMEM = "(unwind-protect form1 [forms]*)\n"
"Evaluates form1 and forms in order and returns the value of form1,\n"
"but guarantees to evaluate forms even if an error occurs in form1.";
const char doc194[] PROGMEM = "(ignore-errors [forms]*)\n"
"Evaluates forms ignoring errors.";
const char doc195[] PROGMEM = "(error controlstring [arguments]*)\n"
"Signals an error. The message is printed by format using the controlstring and arguments.";
const char docG1[] PROGMEM = "(draw-pixel x y [colour])\n"
"Draws a pixel at coordinates (x,y) in colour, or white if omitted.";
const char docG2[] PROGMEM = "(draw-line x0 y0 x1 y1 [colour])\n"
"Draws a line from (x0,y0) to (x1,y1) in colour, or white if omitted.";
const char docG3[] PROGMEM = "(draw-rect x y w h [colour])\n"
"Draws an outline rectangle with its top left corner at (x,y), with width w,\n"
"and with height h. The outline is drawn in colour, or white if omitted.";
const char docG4[] PROGMEM = "(fill-rect x y w h [colour])\n"
"Draws a filled rectangle with its top left corner at (x,y), with width w,\n"
"and with height h. The outline is drawn in colour, or white if omitted.";
const char docG5[] PROGMEM = "(draw-circle x y r [colour])\n"
"Draws an outline circle with its centre at (x, y) and with radius r.\n"
"The circle is drawn in colour, or white if omitted.";
const char docG6[] PROGMEM = "(fill-circle x y r [colour])\n"
"Draws a filled circle with its centre at (x, y) and with radius r.\n"
"The circle is drawn in colour, or white if omitted.";
const char docG7[] PROGMEM = "(draw-triangle x0 y0 x1 y1 x2 y2 [colour])\n"
"Draws an outline triangle between (x1,y1), (x2,y2), and (x3,y3).\n"
"The outline is drawn in colour, or white if omitted.";
const char docG8[] PROGMEM = "(fill-triangle x0 y0 x1 y1 x2 y2 [colour])\n"
"Draws a filled triangle between (x1,y1), (x2,y2), and (x3,y3).\n"
"The outline is drawn in colour, or white if omitted.";
const char docG11[] PROGMEM = "(fill-screen [colour])\n"
"Fills or clears the screen with colour, default black.";
const char docG12[] PROGMEM = "(get-pixel x y)\n"
"Returns the colour of the pixel at x, y.";
const char docG13[] PROGMEM = "(draw-char x y char [colour background size])\n"
"Draws the character char with its top left corner at (x,y).\n"
"The character is drawn in a 5 x 7 pixel font in colour against background,\n"
"which default to white and black respectively.\n"
"The character can optionally be scaled by size.";
const char docG14[] PROGMEM = "(set-cursor x y)\n"
"Sets the start point for text plotting to (x, y).";
const char docG15[] PROGMEM = "(plot [x-intercept y-intercept] [function]...)\n"
"Plots up to four functions on the same graph, optionally with axes.\n"
"Each function should be a function of one parameter, the x coordinate, and it will be called with\n"
"each value of x from 0 to 249. The function should return the y value, from 0 to 121.";
const char docG16[] PROGMEM = "(plot3d [x-intercept y-intercept] [function])\n"
"The function should be a function of two parameters, the x and y coordinates.\n"
"It will be called with each value of x from 0 to 249 and y from 0 to 121\n"
"It should return the colour to be plotted, 0 (black) or 1 (white).";
const char docG17[] PROGMEM = "(check-key char)\n"
"Returns t if the key char is pressed, or nil if not.";
const char docG18[] PROGMEM = "(keyboard enable)\n"
"Disables the keyboard if enable is nil.";

// Built-in symbol lookup table
const tbl_entry_t lookup_table[] PROGMEM = {
  { string0, NULL, 0000, doc0 },
  { string1, NULL, 0000, doc1 },
  { string2, NULL, 0000, doc2 },
  { string3, NULL, 0000, doc3 },
  { string4, NULL, 0000, doc4 },
  { string5, NULL, 0000, NULL },
  { string6, NULL, 0000, NULL },
  { string7, NULL, 0000, NULL },
  { string8, NULL, 0000, NULL },
  { string9, NULL, 0000, doc9 },
  { string10, NULL, 0017, doc10 },
  { string11, NULL, 0017, doc11 },
  { string12, NULL, 0017, doc12 },
  { string13, NULL, 0017, NULL },
  { string14, NULL, 0007, NULL },
  { string15, sp_quote, 0311, NULL },
  { string16, sp_defun, 0327, doc16 },
  { string17, sp_defvar, 0313, doc17 },
  { string18, sp_defcode, 0307, doc18 },
  { string19, fn_eq, 0222, doc19 },
  { string20, fn_car, 0211, doc20 },
  { string21, fn_car, 0211, NULL },
  { string22, fn_cdr, 0211, doc22 },
  { string23, fn_cdr, 0211, NULL },
  { string24, fn_nth, 0222, doc24 },
  { string25, fn_aref, 0227, doc25 },
  { string26, fn_char, 0222, doc26 },
  { string27, fn_stringfn, 0211, doc27 },
  { string28, fn_pinmode, 0222, doc28 },
  { string29, fn_digitalwrite, 0222, doc29 },
  { string30, fn_analogread, 0211, doc30 },
  { string31, fn_analogreference, 0211, doc31 },
  { string32, fn_register, 0212, doc32 },
  { string33, fn_format, 0227, doc33 },
  { string34, sp_or, 0307, doc34 },
  { string35, sp_setq, 0327, doc35 },
  { string36, sp_loop, 0307, doc36 },
  { string37, sp_push, 0322, doc37 },
  { string38, sp_pop, 0311, doc38 },
  { string39, sp_incf, 0312, doc39 },
  { string40, sp_decf, 0312, doc40 },
  { string41, sp_setf, 0327, doc41 },
  { string42, sp_dolist, 0317, doc42 },
  { string43, sp_dotimes, 0317, doc43 },
  { string44, sp_do, 0327, doc44 },
  { string45, sp_dostar, 0317, doc45 },
  { string46, sp_trace, 0301, doc46 },
  { string47, sp_untrace, 0301, doc47 },
  { string48, sp_formillis, 0317, doc48 },
  { string49, sp_time, 0311, doc49 },
  { string50, sp_withoutputtostring, 0317, doc50 },
  { string51, sp_withserial, 0317, doc51 },
  { string52, sp_withi2c, 0317, doc52 },
  { string53, sp_withspi, 0317, doc53 },
  { string54, sp_withsdcard, 0327, doc54 },
  { string55, tf_progn, 0107, doc55 },
  { string56, tf_if, 0123, doc56 },
  { string57, tf_cond, 0107, doc57 },
  { string58, tf_when, 0117, doc58 },
  { string59, tf_unless, 0117, doc59 },
  { string60, tf_case, 0117, doc60 },
  { string61, tf_and, 0107, doc61 },
  { string62, fn_not, 0211, doc62 },
  { string63, fn_not, 0211, NULL },
  { string64, fn_cons, 0222, doc64 },
  { string65, fn_atom, 0211, doc65 },
  { string66, fn_listp, 0211, doc66 },
  { string67, fn_consp, 0211, doc67 },
  { string68, fn_symbolp, 0211, doc68 },
  { string69, fn_arrayp, 0211, doc69 },
  { string70, fn_boundp, 0211, doc70 },
  { string71, fn_keywordp, 0211, doc71 },
  { string72, fn_setfn, 0227, doc72 },
  { string73, fn_streamp, 0211, doc73 },
  { string74, fn_equal, 0222, doc74 },
  { string75, fn_caar, 0211, doc75 },
  { string76, fn_cadr, 0211, doc76 },
  { string77, fn_cadr, 0211, NULL },
  { string78, fn_cdar, 0211, doc78 },
  { string79, fn_cddr, 0211, doc79 },
  { string80, fn_caaar, 0211, doc80 },
  { string81, fn_caadr, 0211, doc81 },
  { string82, fn_cadar, 0211, doc82 },
  { string83, fn_caddr, 0211, doc83 },
  { string84, fn_caddr, 0211, NULL },
  { string85, fn_cdaar, 0211, doc85 },
  { string86, fn_cdadr, 0211, doc86 },
  { string87, fn_cddar, 0211, doc87 },
  { string88, fn_cdddr, 0211, doc88 },
  { string89, fn_length, 0211, doc89 },
  { string90, fn_arraydimensions, 0211, doc90 },
  { string91, fn_list, 0207, doc91 },
  { string92, fn_copylist, 0211, doc92 },
  { string93, fn_makearray, 0215, doc93 },
  { string94, fn_reverse, 0211, doc94 },
  { string95, fn_assoc, 0224, doc95 },
  { string96, fn_member, 0224, doc96 },
  { string97, fn_apply, 0227, doc97 },
  { string98, fn_funcall, 0217, doc98 },
  { string99, fn_append, 0207, doc99 },
  { string100, fn_mapc, 0227, doc100 },
  { string101, fn_mapl, 0227, doc101 },
  { string102, fn_mapcar, 0227, doc102 },
  { string103, fn_mapcan, 0227, doc103 },
  { string104, fn_maplist, 0227, doc104 },
  { string105, fn_mapcon, 0227, doc105 },
  { string106, fn_add, 0207, doc106 },
  { string107, fn_subtract, 0217, doc107 },
  { string108, fn_multiply, 0207, doc108 },
  { string109, fn_divide, 0227, doc109 },
  { string110, fn_divide, 0212, NULL },
  { string111, fn_mod, 0222, doc111 },
  { string112, fn_oneplus, 0211, doc112 },
  { string113, fn_oneminus, 0211, doc113 },
  { string114, fn_abs, 0211, doc114 },
  { string115, fn_random, 0211, doc115 },
  { string116, fn_maxfn, 0217, doc116 },
  { string117, fn_minfn, 0217, doc117 },
  { string118, fn_noteq, 0217, doc118 },
  { string119, fn_numeq, 0217, doc119 },
  { string120, fn_less, 0217, doc120 },
  { string121, fn_lesseq, 0217, doc121 },
  { string122, fn_greater, 0217, doc122 },
  { string123, fn_greatereq, 0217, doc123 },
  { string124, fn_plusp, 0211, doc124 },
  { string125, fn_minusp, 0211, NULL },
  { string126, fn_zerop, 0211, doc126 },
  { string127, fn_oddp, 0211, doc127 },
  { string128, fn_evenp, 0211, doc128 },
  { string129, fn_integerp, 0211, doc129 },
  { string130, fn_integerp, 0211, NULL },
  { string131, fn_charcode, 0211, doc131 },
  { string132, fn_codechar, 0211, doc132 },
  { string133, fn_characterp, 0211, doc133 },
  { string134, fn_stringp, 0211, doc134 },
  { string135, fn_stringeq, 0222, doc135 },
  { string136, fn_stringless, 0222, doc136 },
  { string137, fn_stringgreater, 0222, doc137 },
  { string138, fn_stringnoteq, 0222, doc138 },
  { string139, fn_stringlesseq, 0222, doc139 },
  { string140, fn_stringgreatereq, 0222, doc140 },
  { string141, fn_sort, 0222, doc141 },
  { string142, fn_concatenate, 0217, doc142 },
  { string143, fn_subseq, 0223, doc143 },
  { string144, fn_search, 0224, doc144 },
  { string145, fn_readfromstring, 0211, doc145 },
  { string146, fn_princtostring, 0211, doc146 },
  { string147, fn_prin1tostring, 0211, doc147 },
  { string148, fn_logand, 0207, doc148 },
  { string149, fn_logior, 0207, doc149 },
  { string150, fn_logxor, 0207, doc150 },
  { string151, fn_lognot, 0211, doc151 },
  { string152, fn_ash, 0222, doc152 },
  { string153, fn_logbitp, 0222, doc153 },
  { string154, fn_eval, 0211, doc154 },
  { string155, fn_return, 0201, doc155 },
  { string156, fn_globals, 0200, doc156 },
  { string157, fn_locals, 0200, doc157 },
  { string158, fn_makunbound, 0211, doc158 },
  { string159, fn_break, 0200, doc159 },
  { string160, fn_read, 0201, doc160 },
  { string161, fn_prin1, 0212, doc161 },
  { string162, fn_print, 0212, doc162 },
  { string163, fn_princ, 0212, doc163 },
  { string164, fn_terpri, 0201, doc164 },
  { string165, fn_readbyte, 0202, doc165 },
  { string166, fn_readline, 0201, doc166 },
  { string167, fn_writebyte, 0212, doc167 },
  { string168, fn_writestring, 0212, doc168 },
  { string169, fn_writeline, 0212, doc169 },
  { string170, fn_restarti2c, 0212, doc170 },
  { string171, fn_gc, 0200, doc171 },
  { string172, fn_room, 0200, doc172 },
  { string173, fn_saveimage, 0201, doc173 },
  { string174, fn_loadimage, 0201, doc174 },
  { string175, fn_cls, 0200, doc175 },
  { string176, fn_digitalread, 0211, doc176 },
  { string177, fn_analogreadresolution, 0211, doc177 },
  { string178, fn_analogwrite, 0222, doc178 },
  { string179, fn_dacreference, 0211, doc179 },
  { string180, fn_delay, 0211, doc180 },
  { string181, fn_millis, 0200, doc181 },
  { string182, fn_sleep, 0201, doc182 },
  { string183, fn_note, 0203, doc183 },
  { string184, fn_edit, 0211, doc184 },
  { string185, fn_pprint, 0212, doc185 },
  { string186, fn_pprintall, 0201, doc186 },
  { string187, fn_require, 0211, doc187 },
  { string188, fn_listlibrary, 0200, doc188 },
  { string189, sp_help, 0311, doc189 },
  { string190, fn_documentation, 0212, doc190 },
  { string191, fn_apropos, 0211, doc191 },
  { string192, fn_aproposlist, 0211, doc192 },
  { string193, sp_unwindprotect, 0307, doc193 },
  { string194, sp_ignoreerrors, 0307, doc194 },
  { string195, sp_error, 0317, doc195 },
  { stringG1, fn_drawpixel, 0223, docG1 },
  { stringG2, fn_drawline, 0245, docG2 },
  { stringG3, fn_drawrect, 0245, docG3 },
  { stringG4, fn_fillrect, 0245, docG4 },
  { stringG5, fn_drawcircle, 0234, docG5 },
  { stringG6, fn_fillcircle, 0234, docG6 },
  { stringG7, fn_drawtriangle, 0267, docG7 },
  { stringG8, fn_filltriangle, 0267, docG8 },
  { stringG9, fn_glyphpixel, 0233, NULL },
  { stringG11, fn_fillscreen, 0201, docG11 },
  { stringG12, fn_getpixel, 0222, docG12 },
  { stringG13, fn_drawchar, 0236, docG13 },
  { stringG14, fn_setcursor, 0222, docG14 },
  { stringG15, fn_plot, 0206, docG15 },
  { stringG16, fn_plot3d, 0203, docG16 },
  { stringG17, fn_checkkey, 0211, docG17 },
  { stringG18, fn_keyboard, 0211, docG18 },
  { stringG19, (fn_ptr_type)SHIFT_KEY, 0, NULL },
  { stringG20, (fn_ptr_type)META_KEY, 0, NULL },
  { string196, (fn_ptr_type)LED_BUILTIN, 0, NULL },
  { string197, (fn_ptr_type)HIGH, DIGITALWRITE, NULL },
  { string198, (fn_ptr_type)LOW, DIGITALWRITE, NULL },
  { string199, (fn_ptr_type)INPUT, PINMODE, NULL },
  { string200, (fn_ptr_type)INPUT_PULLUP, PINMODE, NULL },
  { string201, (fn_ptr_type)OUTPUT, PINMODE, NULL },
  { string202, (fn_ptr_type)DEFAULT, ANALOGREFERENCE, NULL },
  { string203, (fn_ptr_type)VDD, ANALOGREFERENCE, NULL },
  { string204, (fn_ptr_type)INTERNAL1V024, ANALOGREFERENCE, NULL },
  { string205, (fn_ptr_type)INTERNAL2V048, ANALOGREFERENCE, NULL },
  { string206, (fn_ptr_type)INTERNAL4V096, ANALOGREFERENCE, NULL },
  { string207, (fn_ptr_type)INTERNAL2V5, ANALOGREFERENCE, NULL },
  { string208, (fn_ptr_type)EXTERNAL, ANALOGREFERENCE, NULL },
  { string209, (fn_ptr_type)ADC_DAC0, ANALOGREAD, NULL },
  { string210, (fn_ptr_type)ADC_TEMPERATURE, ANALOGREAD, NULL },
  { string211, (fn_ptr_type)&PORTA_DIR, REGISTER, NULL },
  { string212, (fn_ptr_type)&PORTA_OUT, REGISTER, NULL },
  { string213, (fn_ptr_type)&PORTA_IN, REGISTER, NULL },
  { string214, (fn_ptr_type)&PORTB_DIR, REGISTER, NULL },
  { string215, (fn_ptr_type)&PORTB_OUT, REGISTER, NULL },
  { string216, (fn_ptr_type)&PORTB_IN, REGISTER, NULL },
  { string217, (fn_ptr_type)&PORTC_DIR, REGISTER, NULL },
  { string218, (fn_ptr_type)&PORTC_OUT, REGISTER, NULL },
  { string219, (fn_ptr_type)&PORTC_IN, REGISTER, NULL },
  { string220, (fn_ptr_type)&PORTD_DIR, REGISTER, NULL },
  { string221, (fn_ptr_type)&PORTD_OUT, REGISTER, NULL },
  { string222, (fn_ptr_type)&PORTD_IN, REGISTER, NULL },
  { string223, (fn_ptr_type)&PORTE_DIR, REGISTER, NULL },
  { string224, (fn_ptr_type)&PORTE_OUT, REGISTER, NULL },
  { string225, (fn_ptr_type)&PORTE_IN, REGISTER, NULL },
  { string226, (fn_ptr_type)&PORTF_DIR, REGISTER, NULL },
  { string227, (fn_ptr_type)&PORTF_OUT, REGISTER, NULL },
  { string228, (fn_ptr_type)&PORTF_IN, REGISTER, NULL },
};

#if !defined(extensions)
// Table cross-reference functions

tbl_entry_t *tables[] = {lookup_table, NULL};
const unsigned int tablesizes[] = { arraysize(lookup_table), 0 };

const tbl_entry_t *table (int n) {
  return tables[n];
}

unsigned int tablesize (int n) {
  return tablesizes[n];
}
#endif

// Table lookup functions

builtin_t lookupbuiltin (char* c) {
  unsigned int end = 0, start;
  for (int n=0; n<2; n++) {
    start = end;
    int entries = tablesize(n);
    end = end + entries;
    for (int i=0; i<entries; i++) {
      if (strcasecmp_P(c, (char*)pgm_read_ptr(&(table(n)[i].string))) == 0)
        return (builtin_t)(start + i);
    }
  }
  return ENDFUNCTIONS;
}

intptr_t lookupfn (builtin_t name) {
  int n = name<tablesize(0);
  return (intptr_t)pgm_read_ptr(&table(n?0:1)[n?name:name-tablesize(0)].fptr);
}

uint8_t getminmax (builtin_t name) {
  int n = name<tablesize(0);
  return pgm_read_byte(&table(n?0:1)[n?name:name-tablesize(0)].minmax);
}

void checkminmax (builtin_t name, int nargs) {
  if (!(name < ENDFUNCTIONS)) error2(PSTR("not a builtin"));
  uint8_t minmax = getminmax(name);
  if (nargs<((minmax >> 3) & 0x07)) error2(toofewargs);
  if ((minmax & 0x07) != 0x07 && nargs>(minmax & 0x07)) error2(toomanyargs);
}

char *lookupdoc (builtin_t name) {
  int n = name<tablesize(0);
  return (char*)pgm_read_ptr(&table(n?0:1)[n?name:name-tablesize(0)].doc);
}

bool findsubstring (char *part, builtin_t name) {
  int n = name<tablesize(0);
  PGM_P s = (char*)pgm_read_ptr(&table(n?0:1)[n?name:name-tablesize(0)].string);
  int l = strlen_P(s);
  int m = strlen(part);
  for (int i = 0; i <= l-m; i++) {
    int j = 0;
    while (j < m && pgm_read_byte(&s[i+j]) == part[j]) j++;
    if (j == m) return true;
  }
  return false;
}

void testescape () {
#if defined serialmonitor
  if (Serial.available() && Serial.read() == '~') error2(PSTR("escape!"));
#endif
  if (tstflag(ESCAPE)) error2(PSTR("escape!"));
}

bool colonp (symbol_t name) {
  if (!longnamep(name)) return false;
  object *form = (object *)name;
  if (form == NULL) return false;
  return (((form->chars)>>((sizeof(int)-1)*8) & 0xFF) == ':');
}

bool keywordp (object *obj) {
  if (!(symbolp(obj) && builtinp(obj->name))) return false;
  builtin_t name = builtin(obj->name);
  int n = name<tablesize(0);
  PGM_P s = (char*)pgm_read_ptr(&table(n?0:1)[n?name:name-tablesize(0)].string);
  char c = pgm_read_byte(s);
  return (c == ':');
}

// Main evaluator

extern char __bss_end[];

object *eval (object *form, object *env) {
  uint8_t sp[0];
  int TC=0;
  EVAL:
  // Enough space?
  //Serial.println((uint16_t)sp - (uint16_t)__bss_end); // Find best STACKDIFF value
  if ((uint16_t)sp - (uint16_t)__bss_end < STACKDIFF) { Context = NIL; error2(PSTR("stack overflow")); }
  if (Freespace <= WORKSPACESIZE>>4) gc(form, env);      // GC when 1/16 of workspace left
  // Escape
  if (tstflag(ESCAPE)) { clrflag(ESCAPE); error2(PSTR("escape!"));}
  if (!tstflag(NOESC)) testescape();

  if (form == NULL) return nil;

  if (form->type >= NUMBER && form->type <= STRING) return form;

  if (symbolp(form)) {
    symbol_t name = form->name;
    if (colonp(name)) return form; // Keyword
    object *pair = value(name, env);
    if (pair != NULL) return cdr(pair);
    pair = value(name, GlobalEnv);
    if (pair != NULL) return cdr(pair);
    else if (builtinp(name)) {
      if (builtin(name) == FEATURES) return features();
      return form;
    }
    Context = NIL;
    error(PSTR("undefined"), form);
  }

  #if defined(CODESIZE)
  if (form->type == CODE) error2(PSTR("can't evaluate CODE header"));
  #endif

  // It's a list
  object *function = car(form);
  object *args = cdr(form);

  if (function == NULL) error(PSTR("illegal function"), nil);
  if (!listp(args)) error(PSTR("can't evaluate a dotted pair"), args);

  // List starts with a builtin symbol?
  if (symbolp(function) && builtinp(function->name)) {
    builtin_t name = builtin(function->name);

    if ((name == LET) || (name == LETSTAR)) {
      int TCstart = TC;
      if (args == NULL) error2(noargument);
      object *assigns = first(args);
      if (!listp(assigns)) error(notalist, assigns);
      object *forms = cdr(args);
      object *newenv = env;
      protect(newenv);
      while (assigns != NULL) {
        object *assign = car(assigns);
        if (!consp(assign)) push(cons(assign,nil), newenv);
        else if (cdr(assign) == NULL) push(cons(first(assign),nil), newenv);
        else push(cons(first(assign),eval(second(assign),env)), newenv);
        car(GCStack) = newenv;
        if (name == LETSTAR) env = newenv;
        assigns = cdr(assigns);
      }
      env = newenv;
      unprotect();
      form = tf_progn(forms,env);
      TC = TCstart;
      goto EVAL;
    }

    if (name == LAMBDA) {
      if (env == NULL) return form;
      object *envcopy = NULL;
      while (env != NULL) {
        object *pair = first(env);
        if (pair != NULL) push(pair, envcopy);
        env = cdr(env);
      }
      return cons(bsymbol(CLOSURE), cons(envcopy,args));
    }
    uint8_t fntype = getminmax(name)>>6;

    if (fntype == SPECIAL_FORMS) {
      Context = name;
      checkargs(args);
      return ((fn_ptr_type)lookupfn(name))(args, env);
    }

    if (fntype == TAIL_FORMS) {
      Context = name;
      checkargs(args);
      form = ((fn_ptr_type)lookupfn(name))(args, env);
      TC = 1;
      goto EVAL;
    }
    if (fntype == OTHER_FORMS) error(PSTR("can't be used as a function"), function);
  }

  // Evaluate the parameters - result in head
  object *fname = car(form);
  int TCstart = TC;
  object *head = cons(eval(fname, env), NULL);
  protect(head); // Don't GC the result list
  object *tail = head;
  form = cdr(form);
  int nargs = 0;

  while (form != NULL){
    object *obj = cons(eval(car(form),env),NULL);
    cdr(tail) = obj;
    tail = obj;
    form = cdr(form);
    nargs++;
  }

  function = car(head);
  args = cdr(head);

  if (symbolp(function)) {
    builtin_t bname = builtin(function->name);
    if (!builtinp(function->name)) error(PSTR("not valid here"), fname);
    Context = bname;
    checkminmax(bname, nargs);
    object *result = ((fn_ptr_type)lookupfn(bname))(args, env);
    unprotect();
    return result;
  }

  if (consp(function)) {
    symbol_t name = sym(NIL);
    if (!listp(fname)) name = fname->name;

    if (isbuiltin(car(function), LAMBDA)) {
      form = closure(TCstart, name, function, args, &env);
      unprotect();
      int trace = tracing(fname->name);
      if (trace) {
        object *result = eval(form, env);
        indent((--(TraceDepth[trace-1]))<<1, ' ', pserial);
        pint(TraceDepth[trace-1], pserial);
        pserial(':'); pserial(' ');
        printobject(fname, pserial); pfstring(PSTR(" returned "), pserial);
        printobject(result, pserial); pln(pserial);
        return result;
      } else {
        TC = 1;
        goto EVAL;
      }
    }

    if (isbuiltin(car(function), CLOSURE)) {
      function = cdr(function);
      form = closure(TCstart, name, function, args, &env);
      unprotect();
      TC = 1;
      goto EVAL;
    }

    #if defined(CODESIZE)
    if (car(function)->type == CODE) {
      int n = listlength(second(function));
      if (nargs<n) errorsym2(fname->name, toofewargs);
      if (nargs>n) errorsym2(fname->name, toomanyargs);
      uint32_t entry = startblock(car(function));
      unprotect();
      return call(entry, n, args, env);
    }
    #endif

  }
  error(PSTR("illegal function"), fname); return nil;
}

// Print functions

void pserial (char c) {
  LastPrint = c;
  if (!tstflag(NOECHO)) Display(c);         // Don't display on LispBadge when paste in listing
  #if defined (serialmonitor)
  if (c == '\n') Serial.write('\r');
  Serial.write(c);
  #endif
}

const char ControlCodes[] PROGMEM = "Null\0SOH\0STX\0ETX\0EOT\0ENQ\0ACK\0Bell\0Backspace\0Tab\0Newline\0VT\0"
"Page\0Return\0SO\0SI\0DLE\0DC1\0DC2\0DC3\0DC4\0NAK\0SYN\0ETB\0CAN\0EM\0SUB\0Escape\0FS\0GS\0RS\0US\0Space\0";

void pcharacter (uint8_t c, pfun_t pfun) {
  if (!tstflag(PRINTREADABLY)) pfun(c);
  else {
    pfun('#'); pfun('\\');
    if (c <= 32) {
      PGM_P p = ControlCodes;
      while (c > 0) {p = p + strlen_P(p) + 1; c--; }
      pfstring(p, pfun);
    } else if (c < 127) pfun(c);
    else pint(c, pfun);
  }
}

void pstring (char *s, pfun_t pfun) {
  while (*s) pfun(*s++);
}

void plispstring (object *form, pfun_t pfun) {
  plispstr(form->name, pfun);
}

void plispstr (symbol_t name, pfun_t pfun) {
  object *form = (object *)name;
  while (form != NULL) {
    int chars = form->chars;
    for (int i=(sizeof(int)-1)*8; i>=0; i=i-8) {
      char ch = chars>>i & 0xFF;
      if (tstflag(PRINTREADABLY) && (ch == '"' || ch == '\\')) pfun('\\');
      if (ch) pfun(ch);
    }
    form = car(form);
  }
}

void printstring (object *form, pfun_t pfun) {
  if (tstflag(PRINTREADABLY)) pfun('"');
  plispstr(form->name, pfun);
  if (tstflag(PRINTREADABLY)) pfun('"');
}

void pbuiltin (builtin_t name, pfun_t pfun) {
  int n = name<tablesize(0);
  PGM_P s = (char*)pgm_read_ptr(&table(n?0:1)[n?name:name-tablesize(0)].string);
  while (1) {
    char c = pgm_read_byte(s++);
    if (c == 0) return;
    pfun(c);
  }
}

void pradix40 (symbol_t name, pfun_t pfun) {
  uint16_t x = untwist(name);
  for (int d=1600; d>0; d = d/40) {
    uint16_t j = x/d;
    char c = fromradix40(j);
    if (c == 0) return;
    pfun(c); x = x - j*d;
  }
}

void printsymbol (object *form, pfun_t pfun) {
  psymbol(form->name, pfun);
}

void psymbol (symbol_t name, pfun_t pfun) {
  if (longnamep(name)) plispstr(name, pfun);
  else {
    uint16_t value = untwist(name);
    if (value < PACKEDS) error2(PSTR("invalid symbol"));
    else if (value >= BUILTINS) pbuiltin((builtin_t)(value-BUILTINS), pfun);
    else pradix40(name, pfun);
  }
}

void pfstring (PGM_P s, pfun_t pfun) {
  while (1) {
    char c = pgm_read_byte(s++);
    if (c == 0) return;
    pfun(c);
  }
}

void pint (int i, pfun_t pfun) {
  uint16_t j = i;
  if (i<0) { pfun('-'); j=-i; }
  pintbase(j, 10, pfun);
}

void pintbase (uint16_t i, uint8_t base, pfun_t pfun) {
  uint8_t lead = 0; uint16_t p = 10000;
  if (base == 2) p = 0x8000; else if (base == 16) p = 0x1000;
  for (uint16_t d=p; d>0; d=d/base) {
    uint16_t j = i/d;
    if (j!=0 || lead || d==1) { pfun((j<10) ? j+'0' : j+'W'); lead=1;}
    i = i - j*d;
  }
}

void printhex2 (int i, pfun_t pfun) {
  for (unsigned int d=0x10; d>0; d=d>>4) {
    unsigned int j = i/d;
    pfun((j<10) ? j+'0' : j+'W');
    i = i - j*d;
  }
}

inline void pln (pfun_t pfun) {
  pfun('\n');
}

void pfl (pfun_t pfun) {
  if (LastPrint != '\n') pfun('\n');
}

void plist (object *form, pfun_t pfun) {
  pfun('(');
  printobject(car(form), pfun);
  form = cdr(form);
  while (form != NULL && listp(form)) {
    pfun(' ');
    printobject(car(form), pfun);
    form = cdr(form);
  }
  if (form != NULL) {
    pfstring(PSTR(" . "), pfun);
    printobject(form, pfun);
  }
  pfun(')');
}

void pstream (object *form, pfun_t pfun) {
  pfun('<');
  PGM_P s = (char*)pgm_read_ptr(&streamname[(form->integer)>>8]);
  pfstring(s, pfun);
  pfstring(PSTR("-stream "), pfun);
  pint(form->integer & 0xFF, pfun);
  pfun('>');
}

void printobject (object *form, pfun_t pfun) {
  if (form == NULL) pfstring(PSTR("nil"), pfun);
  else if (listp(form) && isbuiltin(car(form), CLOSURE)) pfstring(PSTR("<closure>"), pfun);
  else if (listp(form)) plist(form, pfun);
  else if (integerp(form)) pint(form->integer, pfun);
  else if (symbolp(form)) { if (form->name != sym(NOTHING)) printsymbol(form, pfun); }
  else if (characterp(form)) pcharacter(form->chars, pfun);
  else if (stringp(form)) printstring(form, pfun);
  else if (arrayp(form)) printarray(form, pfun);
  #if defined(CODESIZE)
  else if (form->type == CODE) pfstring(PSTR("code"), pfun);
  #endif
  else if (streamp(form)) pstream(form, pfun);
  else error2(PSTR("error in print"));
}

void prin1object (object *form, pfun_t pfun) {
  char temp = Flags;
  clrflag(PRINTREADABLY);
  printobject(form, pfun);
  Flags = temp;
}

// For Lisp Badge
volatile int WritePtr = 0, ReadPtr = 0, LastWritePtr = 0;
const int KybdBufSize = 333; // 42*8 - 3
char KybdBuf[KybdBufSize];
volatile uint8_t KybdAvailable = 0;
volatile uint8_t KybdRawMode = 0;

// Read functions

int glibrary () {
  if (LastChar) {
    char temp = LastChar;
    LastChar = 0;
    return temp;
  }
  char c = pgm_read_byte(&LispLibrary[GlobalStringIndex++]);
  return (c != 0) ? c : -1; // -1?
}

void loadfromlibrary (object *env) {
  GlobalStringIndex = 0;
  object *line = read(glibrary);
  while (line != NULL) {
    protect(line);
    eval(line, env);
    unprotect();
    line = read(glibrary);
  }
}

int gserial () {
  if (LastChar) {
    char temp = LastChar;
    LastChar = 0;
    return temp;
  }
  #if defined (serialmonitor)
  unsigned long start = millis();
  while (!Serial.available() && !KybdAvailable) if (millis() - start > 1000) clrflag(NOECHO);
  if (Serial.available()) {
    char temp = Serial.read();
    if (temp != '\n' && !tstflag(NOECHO)) Serial.print(temp);
    return temp;
  } else {
    if (ReadPtr != WritePtr) {
      char temp = KybdBuf[ReadPtr++];
      Serial.write(temp);
      return temp;
    }
    KybdAvailable = 0;
    WritePtr = 0;
    return '\n';
  }
  #else
  while (!KybdAvailable);
  if (ReadPtr != WritePtr) return KybdBuf[ReadPtr++];
  KybdAvailable = 0;
  WritePtr = 0;
  return '\n';
  #endif
}

object *nextitem (gfun_t gfun) {
  int ch = gfun();
  while(issp(ch)) ch = gfun();

  #if defined(CPU_ATmega328P)
  if (ch == ';') {
    while(ch != '(') ch = gfun();
  }
  #else
  if (ch == ';') {
    do { ch = gfun(); if (ch == ';' || ch == '(') setflag(NOECHO); }
    while(ch != '(');
  }
  #endif
  if (ch == '\n') ch = gfun();
  if (ch == -1) return nil;
  if (ch == ')') return (object *)KET;
  if (ch == '(') return (object *)BRA;
  if (ch == '\'') return (object *)QUO;
  if (ch == '.') return (object *)DOT;

  // Parse string
  if (ch == '"') return readstring('"', true, gfun);

  // Parse symbol, character, or number
  int index = 0, base = 10, sign = 1;
  char buffer[BUFFERSIZE];
  int bufmax = BUFFERSIZE-1; // Max index
  unsigned int result = 0;
  if (ch == '+' || ch == '-') {
    buffer[index++] = ch;
    if (ch == '-') sign = -1;
    ch = gfun();
  }

  // Parse reader macros
  else if (ch == '#') {
    ch = gfun();
    char ch2 = ch & ~0x20; // force to upper case
    if (ch == '\\') { // Character
      base = 0; ch = gfun();
      if (issp(ch) || isbr(ch)) return character(ch);
      else LastChar = ch;
    } else if (ch == '|') {
      do { while (gfun() != '|'); }
      while (gfun() != '#');
      return nextitem(gfun);
    } else if (ch2 == 'B') base = 2;
    else if (ch2 == 'O') base = 8;
    else if (ch2 == 'X') base = 16;
    else if (ch == '\'') return nextitem(gfun);
    else if (ch == '.') {
      setflag(NOESC);
      object *result = eval(read(gfun), NULL);
      clrflag(NOESC);
      return result;
    }
    else if (ch == '(') { LastChar = ch; return readarray(1, read(gfun)); }
    else if (ch == '*') return readbitarray(gfun);
    else if (ch >= '1' && ch <= '9' && (gfun() & ~0x20) == 'A') return readarray(ch - '0', read(gfun));
    else error2(PSTR("illegal character after #"));
    ch = gfun();
  }

  int isnumber = (digitvalue(ch)<base);

  while(!issp(ch) && !isbr(ch) && index < bufmax) {
    buffer[index++] = ch;
    int temp = digitvalue(ch);
    result = result * base + temp;
    isnumber = isnumber && (digitvalue(ch)<base);
    ch = gfun();
  }

  buffer[index] = '\0';
  if (isbr(ch)) LastChar = ch;

  if (isnumber) {
    if (base == 10 && result > ((unsigned int)INT_MAX+(1-sign)/2))
      error2(PSTR("Number out of range"));
    return number(result*sign);
  } else if (base == 0) {
    if (index == 1) return character(buffer[0]);
    PGM_P p = ControlCodes; char c = 0;
    while (c < 33) {
      if (strcasecmp_P(buffer, p) == 0) return character(c);
      p = p + strlen_P(p) + 1; c++;
    }
    if (index == 3) return character((buffer[0]*10+buffer[1])*10+buffer[2]-5328);
    error2(PSTR("unknown character"));
  }

  builtin_t x = lookupbuiltin(buffer);
  if (x == NIL) return nil;
  if (x != ENDFUNCTIONS) return bsymbol(x);
  if (index <= 3 && valid40(buffer)) return intern(twist(pack40(buffer)));
  return internlong(buffer);
}

object *readrest (gfun_t gfun) {
  object *item = nextitem(gfun);
  object *head = NULL;
  object *tail = NULL;

  while (item != (object *)KET) {
    if (item == (object *)BRA) {
      item = readrest(gfun);
    } else if (item == (object *)QUO) {
      item = cons(bsymbol(QUOTE), cons(read(gfun), NULL));
    } else if (item == (object *)DOT) {
      tail->cdr = read(gfun);
      if (readrest(gfun) != NULL) error2(PSTR("malformed list"));
      return head;
    } else {
      object *cell = cons(item, NULL);
      if (head == NULL) head = cell;
      else tail->cdr = cell;
      tail = cell;
      item = nextitem(gfun);
    }
  }
  return head;
}

object *read (gfun_t gfun) {
  object *item = nextitem(gfun);
  if (item == (object *)KET) error2(PSTR("incomplete list"));
  if (item == (object *)BRA) return readrest(gfun);
  if (item == (object *)DOT) return read(gfun);
  if (item == (object *)QUO) return cons(bsymbol(QUOTE), cons(read(gfun), NULL));
  return item;
}

// Lisp Badge terminal and keyboard support

// These are the bit positions in PORTF
int const sck = 2;   // PF2
int const mosi = 3;  // PF3
int const dc = 4;    // PF4
int const cs = 5;    // PF5

// ATtiny 0-, 1-, and 2-series port manipulations - assumes all pins in same port
#define PORT_TOGGLE(x)  PORTF.OUTTGL = (x)
#define PORT_LOW(x)     PORTF.OUTCLR = (x)
#define PORT_HIGH(x)    PORTF.OUTSET = (x)
#define PORT_OUTPUT(x)  PORTF.DIRSET = (x)
#define PORT_INPUT(x)   PORTF.DIRCLR = (x)
#define PORT_IN         PORTF.IN

// Terminal **********************************************************************************

// Character set - stored in program memory
const uint8_t CharMap[96][6] PROGMEM = {
{ 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },
{ 0x00, 0x00, 0x5F, 0x00, 0x00, 0x00 },
{ 0x00, 0x07, 0x00, 0x07, 0x00, 0x00 },
{ 0x14, 0x7F, 0x14, 0x7F, 0x14, 0x00 },
{ 0x24, 0x2A, 0x7F, 0x2A, 0x12, 0x00 },
{ 0x23, 0x13, 0x08, 0x64, 0x62, 0x00 },
{ 0x36, 0x49, 0x56, 0x20, 0x50, 0x00 },
{ 0x00, 0x08, 0x07, 0x03, 0x00, 0x00 },
{ 0x00, 0x1C, 0x22, 0x41, 0x00, 0x00 },
{ 0x00, 0x41, 0x22, 0x1C, 0x00, 0x00 },
{ 0x2A, 0x1C, 0x7F, 0x1C, 0x2A, 0x00 },
{ 0x08, 0x08, 0x3E, 0x08, 0x08, 0x00 },
{ 0x00, 0x80, 0x70, 0x30, 0x00, 0x00 },
{ 0x08, 0x08, 0x08, 0x08, 0x08, 0x00 },
{ 0x00, 0x00, 0x60, 0x60, 0x00, 0x00 },
{ 0x20, 0x10, 0x08, 0x04, 0x02, 0x00 },
{ 0x3E, 0x51, 0x49, 0x45, 0x3E, 0x00 },
{ 0x00, 0x42, 0x7F, 0x40, 0x00, 0x00 },
{ 0x72, 0x49, 0x49, 0x49, 0x46, 0x00 },
{ 0x21, 0x41, 0x49, 0x4D, 0x33, 0x00 },
{ 0x18, 0x14, 0x12, 0x7F, 0x10, 0x00 },
{ 0x27, 0x45, 0x45, 0x45, 0x39, 0x00 },
{ 0x3C, 0x4A, 0x49, 0x49, 0x31, 0x00 },
{ 0x41, 0x21, 0x11, 0x09, 0x07, 0x00 },
{ 0x36, 0x49, 0x49, 0x49, 0x36, 0x00 },
{ 0x46, 0x49, 0x49, 0x29, 0x1E, 0x00 },
{ 0x00, 0x36, 0x36, 0x00, 0x00, 0x00 },
{ 0x00, 0x56, 0x36, 0x00, 0x00, 0x00 },
{ 0x00, 0x08, 0x14, 0x22, 0x41, 0x00 },
{ 0x14, 0x14, 0x14, 0x14, 0x14, 0x00 },
{ 0x00, 0x41, 0x22, 0x14, 0x08, 0x00 },
{ 0x02, 0x01, 0x59, 0x09, 0x06, 0x00 },
{ 0x3E, 0x41, 0x5D, 0x59, 0x4E, 0x00 },
{ 0x7C, 0x12, 0x11, 0x12, 0x7C, 0x00 },
{ 0x7F, 0x49, 0x49, 0x49, 0x36, 0x00 },
{ 0x3E, 0x41, 0x41, 0x41, 0x22, 0x00 },
{ 0x7F, 0x41, 0x41, 0x41, 0x3E, 0x00 },
{ 0x7F, 0x49, 0x49, 0x49, 0x41, 0x00 },
{ 0x7F, 0x09, 0x09, 0x09, 0x01, 0x00 },
{ 0x3E, 0x41, 0x41, 0x51, 0x73, 0x00 },
{ 0x7F, 0x08, 0x08, 0x08, 0x7F, 0x00 },
{ 0x00, 0x41, 0x7F, 0x41, 0x00, 0x00 },
{ 0x20, 0x40, 0x41, 0x3F, 0x01, 0x00 },
{ 0x7F, 0x08, 0x14, 0x22, 0x41, 0x00 },
{ 0x7F, 0x40, 0x40, 0x40, 0x40, 0x00 },
{ 0x7F, 0x02, 0x1C, 0x02, 0x7F, 0x00 },
{ 0x7F, 0x04, 0x08, 0x10, 0x7F, 0x00 },
{ 0x3E, 0x41, 0x41, 0x41, 0x3E, 0x00 },
{ 0x7F, 0x09, 0x09, 0x09, 0x06, 0x00 },
{ 0x3E, 0x41, 0x51, 0x21, 0x5E, 0x00 },
{ 0x7F, 0x09, 0x19, 0x29, 0x46, 0x00 },
{ 0x26, 0x49, 0x49, 0x49, 0x32, 0x00 },
{ 0x03, 0x01, 0x7F, 0x01, 0x03, 0x00 },
{ 0x3F, 0x40, 0x40, 0x40, 0x3F, 0x00 },
{ 0x1F, 0x20, 0x40, 0x20, 0x1F, 0x00 },
{ 0x3F, 0x40, 0x38, 0x40, 0x3F, 0x00 },
{ 0x63, 0x14, 0x08, 0x14, 0x63, 0x00 },
{ 0x03, 0x04, 0x78, 0x04, 0x03, 0x00 },
{ 0x61, 0x59, 0x49, 0x4D, 0x43, 0x00 },
{ 0x00, 0x7F, 0x41, 0x41, 0x41, 0x00 },
{ 0x02, 0x04, 0x08, 0x10, 0x20, 0x00 },
{ 0x00, 0x41, 0x41, 0x41, 0x7F, 0x00 },
{ 0x04, 0x02, 0x01, 0x02, 0x04, 0x00 },
{ 0x40, 0x40, 0x40, 0x40, 0x40, 0x00 },
{ 0x00, 0x03, 0x07, 0x08, 0x00, 0x00 },
{ 0x20, 0x54, 0x54, 0x78, 0x40, 0x00 },
{ 0x7F, 0x28, 0x44, 0x44, 0x38, 0x00 },
{ 0x38, 0x44, 0x44, 0x44, 0x28, 0x00 },
{ 0x38, 0x44, 0x44, 0x28, 0x7F, 0x00 },
{ 0x38, 0x54, 0x54, 0x54, 0x18, 0x00 },
{ 0x00, 0x08, 0x7E, 0x09, 0x02, 0x00 },
{ 0x18, 0xA4, 0xA4, 0x9C, 0x78, 0x00 },
{ 0x7F, 0x08, 0x04, 0x04, 0x78, 0x00 },
{ 0x00, 0x44, 0x7D, 0x40, 0x00, 0x00 },
{ 0x20, 0x40, 0x40, 0x3D, 0x00, 0x00 },
{ 0x7F, 0x10, 0x28, 0x44, 0x00, 0x00 },
{ 0x00, 0x41, 0x7F, 0x40, 0x00, 0x00 },
{ 0x7C, 0x04, 0x78, 0x04, 0x78, 0x00 },
{ 0x7C, 0x08, 0x04, 0x04, 0x78, 0x00 },
{ 0x38, 0x44, 0x44, 0x44, 0x38, 0x00 },
{ 0xFC, 0x18, 0x24, 0x24, 0x18, 0x00 },
{ 0x18, 0x24, 0x24, 0x18, 0xFC, 0x00 },
{ 0x7C, 0x08, 0x04, 0x04, 0x08, 0x00 },
{ 0x48, 0x54, 0x54, 0x54, 0x24, 0x00 },
{ 0x04, 0x04, 0x3F, 0x44, 0x24, 0x00 },
{ 0x3C, 0x40, 0x40, 0x20, 0x7C, 0x00 },
{ 0x1C, 0x20, 0x40, 0x20, 0x1C, 0x00 },
{ 0x3C, 0x40, 0x30, 0x40, 0x3C, 0x00 },
{ 0x44, 0x28, 0x10, 0x28, 0x44, 0x00 },
{ 0x4C, 0x90, 0x90, 0x90, 0x7C, 0x00 },
{ 0x44, 0x64, 0x54, 0x4C, 0x44, 0x00 },
{ 0x00, 0x08, 0x36, 0x41, 0x00, 0x00 },
{ 0x00, 0x00, 0x77, 0x00, 0x00, 0x00 },
{ 0x00, 0x41, 0x36, 0x08, 0x00, 0x00 },
{ 0x02, 0x01, 0x02, 0x04, 0x02, 0x00 },
{ 0xC0, 0xC0, 0xC0, 0xC0, 0xC0, 0xC0 }
};

// Monochrome display **********************************************

int const CASET = 0x2A; // Define column address
int const RASET = 0x2B; // Define row address
int const RAMWR = 0x2C; // Write to display RAM
int const RAMRD = 0x2E; // Read from display RAM

// Current plot position and colours
// uint8_t scale = 1;     // Text scale
boolean smooth = true;

// Send a byte to the display

void Data (uint8_t d) {
  for (uint8_t bit = 0x80; bit; bit >>= 1) {
    PORT_TOGGLE(1<<sck);
    if (d & bit) PORT_HIGH(1<<mosi); else PORT_LOW(1<<mosi);
    PORT_TOGGLE(1<<sck);
  }
}

// Send a command to the display
void Command (uint8_t c) {
  PORT_TOGGLE(1<<dc);
  Data(c);
  PORT_TOGGLE(1<<dc);
}

// Send a command followed by two data bytes
void Command2 (uint8_t c, uint8_t d1, uint8_t d2) {
  Command(c); Data(d1); Data(d2);
}

void InitDisplay () {
  PORT_OUTPUT(1<<dc | 1<<cs | 1<<mosi | 1<<sck); // All outputs
  PORT_HIGH(1<<dc | 1<<cs | 1<<sck);       // Outputs high
  PORT_TOGGLE(1<<cs);
  Command(0x01);                           // Software reset
  delay(250);                              // delay 250 ms
  Command(0xEB); Data(0x02);               // Enable NVM
  Command(0xD7); Data(0x68);               // NVM Load Control
  Command(0xD1); Data(0x01);               // Booster enable
  Command(0xC0); Data(0x80);               // Gate Voltage Setting VGH=12V; VGL=-5V
  Command(0xC1);                           // Source Voltage Control 1
  Data(0x28); Data(0x28); Data(0x28); Data(0x28); Data(0x14); Data(0x00);
  Command(0xC2);                           // Source Voltage Control 2
  Command(0xCB); Data(0x14);               // VCOMH Voltage Setting 4V
  Command(0xB4);                           // Update Period Gate EQ Control
  Data(0xE5); Data(0x77); Data(0xF1); Data(0xFF); Data(0xFF); Data(0x4F);
  Data(0xF1); Data(0xFF); Data(0xFF); Data(0x4F);
  Command(0xB0); Data(0x64);               // Duty Setting
  Command(0x11);                           // Out of sleep mode
  delay(100);
  Command(0xC7); Data(0xA6); Data(0xE9);   // OSC Enable
  Command(0x36); Data(0x20);               // Memory Data Access Control
  Command(0x3A); Data(0x11);               // Data Format Select
  Command(0xB8); Data(0x09);               // Panel Setting
  Command(0xD0); Data(0x1F);               // Unknown command??
  Command(0x21);                           // Display Inversion On
  Command(0x29);                           // Display on
  Command(0xB9); Data(0xE3);               // Clear RAM
  delay(100);
  Command(0xB9); Data(0x23);               // Source Setting Off
  Command(0x72); Data(0x00);               // Destress Off??
  Command(0x39);                           // Low Power Mode ON
  delay(100);
  PORT_TOGGLE(1<<cs);
}

void InvertDisplay (uint8_t yes) {
  PORT_TOGGLE(1<<cs);
  Command(0x20 + yes);
  PORT_TOGGLE(1<<cs);
}

void SleepDisplay (uint8_t yes) {
  PORT_TOGGLE(1<<cs);
  Command(0x11 - yes);
  PORT_TOGGLE(1<<cs);
}

// Character terminal **********************************************

const int Columns = 41;
const int Lines = 10;
const int LastColumn = Columns-1;
const int LastLine = Lines-1;

// Optimised for fast scrolling
void ClearLine (uint8_t line) {
  PORT_TOGGLE(1<<cs);
  Command2(CASET, 25+line, 25+line);   // Column Address Set
  Command2(RASET, 0, 124);   // Row Address Set
  Command(RAMWR);
  for (int i=0; i<375*8; i++) {
    PORT_TOGGLE(1<<sck);
    PORT_TOGGLE(1<<sck);
  }
  PORT_TOGGLE(1<<cs);
}

void ClearDisplay (uint8_t grey) {
  PORT_TOGGLE(1<<cs);
  Command2(CASET, 25, 35);   // Column Address Set
  Command2(RASET, 0, 124);   // Row Address Set
  Command(RAMWR);
  for (int i=0; i<4125; i++) Data(grey);
  PORT_TOGGLE(1<<cs);
}

// Plot a 12x2 block at row, col
void PlotBlock (uint32_t block, uint8_t column, uint8_t line) {
  PORT_TOGGLE(1<<cs);
  Command2(CASET, 25+line, 25+line);
  Command2(RASET, column, column);
  Command(RAMWR); Data(block>>16); Data(block>>8); Data(block);
  PORT_TOGGLE(1<<cs);
}

// Read a 12x2 block at row, col
uint32_t ReadBlock (uint8_t column, uint8_t line) {
  uint32_t pix = 0;
  PORT_TOGGLE(1<<cs);
  Command2(CASET, 25+line, 25+line);
  Command2(RASET, column, column);
  Command(RAMRD);
  PORT_INPUT(1<<mosi);                     // mosi input
  for (uint8_t i=0; i<25; i++) {
    PORT_TOGGLE(1<<sck);
    pix = pix<<1 | (PORT_IN>>mosi & 1);
    PORT_TOGGLE(1<<sck);
  }
  PORT_OUTPUT(1<<mosi);                    // mosi output
  PORT_TOGGLE(1<<cs);
  return pix;
}

// Converts bit pattern abcdefgh into a0b0c0d0e0f0g0h
uint16_t Stretch (uint8_t b) {
  uint16_t x = b;
  x = (x & 0xF0)<<4 | (x & 0x0F);
  x = (x<<2 | x) & 0x3333;
  x = (x<<1 | x) & 0x5555;
  return x;
}

uint16_t Interleave (uint16_t x, uint16_t y) {
  return Stretch(x)<<1 | Stretch(y);
}

uint8_t Reverse (uint8_t x) {
  x = ((x >> 1) & 0x55) | ((x << 1) & 0xaa);
  x = ((x >> 2) & 0x33) | ((x << 2) & 0xcc);
  x = ((x >> 4) & 0x0f) | ((x << 4) & 0xf0);
  return x;
}

// Scrolls the display up by one line, then clears the bottom line
void ScrollDisplay () {
  uint32_t block, block2;
  for (uint8_t x = 0; x < Columns*3; x++) {
    block = ReadBlock(x, 0);
    for (uint8_t y = 1; y < Lines; y++) {
      block2 = ReadBlock(x, y);
      if (block2 != block) {                       // Optimisation if already the same
        PlotBlock(block2, x, y-1);
        block = block2;
      }
    }
  }
  ClearLine(LastLine);
}

// Move current plot position to x,y
void MoveTo (uint8_t x, uint8_t y) {
  Xpos = x; Ypos = y;
}

void PlotPoint (uint8_t x, uint8_t y, uint8_t colour) {
  static uint8_t row0 = 255/2, column0 = 0;
  static uint32_t pixels;
  if (y > 121) return;
  uint8_t v = 121 - y;
  uint8_t row = x/2, column = v/12;
  uint8_t bit = ((~x)&1) | (11 - v%12)<<1;
  if (row != row0 || column != column0) {
    PlotBlock(pixels, row0, column0);
    if (row != 255/2) pixels = ReadBlock(row, column);
    row0 = row; column0 = column;
  }
  pixels = colour ? pixels | (uint32_t)1<<bit : pixels & ~((uint32_t)1<<bit) ;
}

// Draw a line from Xpos,Ypos to x,y
void DrawTo (uint8_t x, uint8_t y, uint8_t colour) {
  int sx, sy, e2, err;
  int dx = abs(x - Xpos);
  int dy = abs(y - Ypos);
  if (Xpos < x) sx = 1; else sx = -1;
  if (Ypos < y) sy = 1; else sy = -1;
  err = dx - dy;
  for (;;) {
    PlotPoint(Xpos, Ypos, colour);
    if (Xpos==x && Ypos==y) break;
    e2 = err<<1;
    if (e2 > -dy) { err = err - dy; Xpos = Xpos + sx; }
    if (e2 < dx) { err = err + dx; Ypos = Ypos + sy; }
  }
  PlotPoint(255, 0, colour); // Flush
}

void DrawRect (uint8_t w, uint8_t h, uint8_t colour) {
  uint8_t x = Xpos, y = Ypos;
  MoveTo(x, y); DrawTo(x+w-1, y, colour);
  DrawTo(x+w-1, y+h-1, colour); DrawTo(x, y+h-1, colour);
  DrawTo(x, y, colour);
}

void FillRect (uint8_t w, uint8_t h, uint8_t colour) {
  uint8_t x = Xpos, y = Ypos;
  for (uint8_t i=x; i<x+w; i++) {
    MoveTo(i, y); DrawTo(i, y-h+1, colour);
  }
  Xpos = x; Ypos = y;
}

void DrawCircle (uint8_t radius, uint8_t colour) {
  uint8_t x1 = Xpos, y1 = Ypos; int dx = 1, dy = 1;
  uint8_t x = radius - 1, y = 0;
  int err = dx - (radius<<1);
  while (x >= y) {
    PlotPoint(x1+x, y1-y, colour); PlotPoint(x1+x, y1+y, colour); //4
    PlotPoint(x1+y, y1-x, colour); PlotPoint(x1+y, y1+x, colour); //3
    PlotPoint(x1-y, y1-x, colour); PlotPoint(x1-y, y1+x, colour); //2
    PlotPoint(x1-x, y1-y, colour); PlotPoint(x1-x, y1+y, colour); //1
    if (err > 0) {
      x = x - 1; dx = dx + 2;
      err = err - (radius<<1) + dx;
    } else {
      y = y + 1; err = err + dy;
      dy = dy + 2;
    }
  }
  Xpos = x1; Ypos = y1;
}

void FillCircle (uint8_t radius, uint8_t colour) {
  uint8_t x1 = Xpos, y1 = Ypos; int dx = 1, dy = 1;
  uint8_t x = radius - 1, y = 0;
  int err = dx - (radius<<1);
  while (x >= y) {
    MoveTo(x1+x, y1-y); DrawTo(x1+x, y1+y, colour); //4
    MoveTo(x1+y, y1-x); DrawTo(x1+y, y1+x, colour); //3
    MoveTo(x1-y, y1-x); DrawTo(x1-y, y1+x, colour); //2
    MoveTo(x1-x, y1-y); DrawTo(x1-x, y1+y, colour); //1
    if (err > 0) {
      x = x - 1; dx = dx + 2;
      err = err - (radius<<1) + dx;
    } else {
      y = y + 1; err = err + dy;
      dy = dy + 2;
    }
  }
  Xpos = x1; Ypos = y1;
}

#define swap(a, b) { a = a ^ b; b = b ^ a; a = a ^ b; }

void DrawTriangle(uint16_t x0, uint16_t y0, uint16_t x1,
                  uint16_t y1, uint16_t x2, uint16_t y2, uint8_t colour) {
  MoveTo(x0, y0); DrawTo(x1, y1, colour); DrawTo(x2, y2, colour); DrawTo(x0, y0, colour);
}

void FillTriangle(int16_t x0, int16_t y0, int16_t x1,
                  int16_t y1, int16_t x2, int16_t y2, uint8_t colour) {
  // Sort coordinates by y order (y2 >= y1 >= y0)
  if (y0 > y1) { swap(y0, y1); swap(x0, x1); }
  if (y1 > y2) { swap(y1, y2); swap(x1, x2); }
  if (y0 > y1) { swap(y0, y1); swap(x0, x1); }
  TriangleQuad(x0, y0, x1, y1, x2, y2, x2, y2, colour);
}

void TriangleQuad(int16_t x0, int16_t y0, int16_t x1, int16_t y1,
                  int16_t x2, int16_t y2, int16_t x3, int16_t y3, uint8_t colour) {
  // Coordinates already in y order (y3 >= y2 >= y1 >= y0)
  int16_t a, b, y;

  // Special case?
  int16_t x4 = x0 + (x3 - x0) * (y1 - y0) / (y3 - y0);
  int16_t x5 = x0 + (x3 - x0) * (y2 - y0) / (y3 - y0);

  if ((x5 > x2) == (x4 > x1)) {
    swap(x2, x5);
  } else { // Kite
    x4 = x0 + (x2 - x0) * (y1 - y0) / (y2 - y0);
    x5 = x1 + (x3 - x1) * (y2 - y1) / (y3 - y1);
  }

  // Fill bottom section
  for (y = y0; y <= y1; y++) {
    a = x0 + (x4 - x0) * (y - y0) / (y1 - y0);
    b = x0 + (x1 - x0) * (y - y0) / (y1 - y0);
    if (a > b) swap(a, b);
    MoveTo(a, y); FillRect(b - a + 1, 1, colour);
  }

  // Fill middle section
  for (; y <= y2; y++) {
    a = x4 + (x2 - x4) * (y - y1) / (y2 - y1);
    b = x1 + (x5 - x1) * (y - y1) / (y2 - y1);
    if (a > b) swap(a, b);
    MoveTo(a, y); FillRect(b - a + 1, 1, colour);
  }

  // Fill top section
  for (; y <= y3; y++) {
    a = x2 + (x3 - x2) * (y - y2) / (y3 - y2);
    b = x5 + (x3 - x5) * (y - y2) / (y3 - y2);
    if (a > b) swap(a, b);
    MoveTo(a, y); FillRect(b - a + 1, 1, colour);
  }
}

// Plot character - vertical position is aligned to nearest 12 pixels for efficiency
void PlotChar (uint8_t ch, uint8_t line, uint8_t column) {
  uint8_t inv = (ch & 0x80) ? 0xFE : 0;                  // Parenthesis highlight
  ch = (ch & 0x7f) - 32;
  PORT_TOGGLE(1<<cs);
  Command2(CASET, 25+line, 25+line);                     // Column Address Set
  Command2(RASET, (column*3), (column*3)+2);             // Row Address Set
  Command(RAMWR);
  for (int xx=0; xx<3; xx++) {
    uint8_t left = Reverse(pgm_read_byte(&CharMap[ch][xx*2])) ^ inv;
    uint8_t right = Reverse(pgm_read_byte(&CharMap[ch][xx*2+1])) ^ inv;
    uint16_t data = Interleave(left, right);
    Data(0); Data(data>>8 & 0xFF); Data(data & 0xFF);
  }
  PORT_TOGGLE(1<<cs);
}

// Prints a character to display, with cursor, handling control characters
void Display (char c) {
  static uint8_t Line = 0, Column = 0, Scroll = 0;
  // These characters don't affect the cursor
  if (c == 8) {                    // Backspace
    if (Column == 0) {
      Line--; Column = LastColumn;
    } else Column--;
    return;
  }
  if (c == 9) {                    // Cursor forward
    if (Column == LastColumn) {
      Line++; Column = 0;
    } else Column++;
    return;
  }
  if ((c >= 17) && (c <= 20)) {    // Parentheses
    if (c == 17) PlotChar('(', Line+Scroll, Column);
    else if (c == 18) PlotChar('(' | 0x80, Line+Scroll, Column);
    else if (c == 19) PlotChar(')', Line+Scroll, Column);
    else PlotChar(')' | 0x80, Line+Scroll, Column);
    return;
  }
  // Hide cursor
  PlotChar(' ', Line+Scroll, Column);
  if (c == 0x7F) {                 // DEL
    if (Column == 0) {
      Line--; Column = LastColumn;
    } else Column--;
  } else if ((c & 0x7f) >= 32) {   // Normal character
    PlotChar(c, Line+Scroll, Column++);
    if (Column > LastColumn) {
      Column = 0;
      if (Line == LastLine) ScrollDisplay(); else Line++;
    }
  // Control characters
  } else if (c == 12) {            // Clear display
    ClearDisplay(0); Line = 0; Column = 0;
  } else if (c == '\n') {          // Newline
    Column = 0;
    if (Line == LastLine) ScrollDisplay(); else Line++;
  } else if (c == 7) tone(12, 440, 125); // Beep
  // Show cursor
  PlotChar(0x7F, Line+Scroll, Column);
}

// Keyboard **********************************************************************************

const int ColumnsE = 0b00000111;            // Columns 0 to 2 in port E
const int ColumnsC = 0b11111111;            // Columns 3 to 10 in port C
const int RowBits  = 0b00001111;            // Rows 0 to 4 in port B

// Character set - stored in program memory
const char Keymap[] PROGMEM =
// Without shift
"1234567890\b" "qwertyuiop\n" "asdfghjkl?\e" " zxcvbnm()."
// With shift
"\'\"#=-+/*\\;\b" "QWERTYUIOP\t" "ASDFGHJKL?~" "?ZXCVBNM<>,"
// With meta
"\'\"#=-+/*\\;\b" "?W!R@Y^IO%\n" "&S$FGHJKL?~" "?ZX:|BNM{},";

bool checkkey (char key) {
  for (uint8_t k=0; k<44; k++) {
    if (pgm_read_byte(&Keymap[k]) == key) {
      uint8_t column = k % 11;
      if (column < 3) PORTE.OUTCLR = 1<<(2-column); else PORTC.OUTCLR = 1<<(10-column);
      uint8_t row = 3 - k/11; // Gives port time to settle
      uint8_t input = PORTB.IN;
      if (column < 3) PORTE.OUTSET = 1<<(2-column); else PORTC.OUTSET = 1<<(10-column);
      return ((input & 1<<row) == 0);
    }
  }
  return false;
}

// Parenthesis highlighting
void Highlight (int p, uint8_t invert) {
  if (p) {
    for (int n=0; n < p; n++) Display(8);
    Display(17 + invert);
    for (int n=1; n < p; n++) Display(9);
    Display(19 + invert);
    Display(9);
  }
}

ISR(TCB3_INT_vect) {
  TCB3.INTFLAGS = TCB_CAPT_bm;                        // Clear the interrupt flag
  static uint8_t column = 0, nokey = 0;
  uint8_t rows, shift, meta, row;
  // Check rows and shift key
  shift = (PORTE.IN & 1<<3) ? 0 : 1;
  meta = (PORTB.IN & 1<<5) ? 0 : 1;
  rows = PORTB.IN & RowBits;
  if (rows == RowBits) { if (nokey < 11) nokey++; }   // No key pressed
  else if (nokey < 11) nokey = 0;                     // Key debounce
  else {
    nokey = 0; row = 0;
    while ((rows & (1<<row)) != 0) row++;
    char c = pgm_read_byte(&Keymap[(3-row)*11 + column + 44*shift + 88*meta]); // TBD Shift+Meta
    if (KybdRawMode) {
      ProcessKeyRaw(c);
    } else {
      ProcessKey(c);
    }
  }
  // Take this column high and next column low
  if (column < 3) PORTE.OUTSET = 1<<(2-column); else PORTC.OUTSET = 1<<(10-column);
  column = (column + 1) % 11;   // 0 to 10
  if (column < 3) PORTE.OUTCLR = 1<<(2-column); else PORTC.OUTCLR = 1<<(10-column);
}

void keyboard (bool enable) {
  if (enable) TCB3.INTCTRL = TCB_CAPT_bm;
  else {
    TCB3.INTCTRL = 0;
    PORTE.OUTSET = ColumnsE;  // Take columns high
    PORTC.OUTSET = ColumnsC;
  }
}

void ProcessKey (char c) {
  static int parenthesis = 0;
  static bool string = false;
  if (c == 27) { setflag(ESCAPE); return; }    // Escape key
  // Undo previous parenthesis highlight
  Highlight(parenthesis, 0);
  parenthesis = 0;
  // Edit buffer
  if (c == '\n') {
    pserial('\n');
    KybdAvailable = 1;
    ReadPtr = 0; LastWritePtr = WritePtr;
    return;
  }
  if (c == 8) {     // Backspace key
    if (WritePtr > 0) {
      WritePtr--;
      Display(0x7F);
      if (WritePtr) c = KybdBuf[WritePtr-1];
    }
  } else if (c == 9) { // tab or ctrl-I
    for (int i = 0; i < LastWritePtr; i++) Display(KybdBuf[i]);
    WritePtr = LastWritePtr;
  } else if (WritePtr < KybdBufSize) {
    if (c == '"') string = !string;
    KybdBuf[WritePtr++] = c;
    Display(c);
  }
  // Do new parenthesis highlight
  if (c == ')' && !string) {
    int search = WritePtr-1, level = 0, string2 = false;
    while (search >= 0 && parenthesis == 0) {
      c = KybdBuf[search--];
      if (c == '"') string2 = !string2;
      if (c == ')' && !string2) level++;
      if (c == '(' && !string2) {
        level--;
        if (level == 0) parenthesis = WritePtr-search-1;
      }
    }
    Highlight(parenthesis, 1);
  }
  return;
}


void SetKeyboardRaw(int raw) {
  KybdRawMode = raw;
  WritePtr = 0;
  ReadPtr = 0;
}

void ProcessKeyRaw (char c) {
  if (WritePtr < KybdBufSize) {
    KybdBuf[WritePtr++] = c;
  }
}

char GetRawKey() {
  // TODO only returns latest key.  Better would be
  // to return the first char and shift the rest of
  // the buffer down.
  if (WritePtr > 0) {
    WritePtr--;
    return KybdBuf[WritePtr];
  } else {
    return 0;
  }
}

void InitKybd () {
  // Make rows input pullups
  PORTB.PIN0CTRL = PORT_PULLUPEN_bm;
  PORTB.PIN1CTRL = PORT_PULLUPEN_bm;
  PORTB.PIN2CTRL = PORT_PULLUPEN_bm;
  PORTB.PIN3CTRL = PORT_PULLUPEN_bm;
  // Make meta and shift keys input pullup
  PORTE.PIN3CTRL = PORT_PULLUPEN_bm;
  PORTB.PIN5CTRL = PORT_PULLUPEN_bm;
  // Make columns outputs
  PORTE.DIRSET = ColumnsE;                            // Columns 0 to 2
  PORTC.DIRSET = ColumnsC;                            // Columns 3 to 10
  // Take columns high
  PORTE.OUTSET = ColumnsE;                            // Columns 0 to 2
  PORTC.OUTSET = ColumnsC;                            // Columns 3 to 10
  // Set up Timer/Counter TCB3 to multiplex the keyboard (tone() is on TCB0)
  TCB3.CCMP = (unsigned int)(F_CPU/250 - 1);          // Divide clock to give 250Hz
  TCB3.CTRLA = TCB_CLKSEL_DIV1_gc | TCB_ENABLE_bm;    // Enable timer, divide by 1
  TCB3.CTRLB = 0;                                     // Periodic Interrupt Mode
  TCB3.INTCTRL = TCB_CAPT_bm;                         // Enable interrupt
}

// Setup

void initenv () {
  GlobalEnv = NULL;
  tee = bsymbol(TEE);
}

// Entry point from the Arduino IDE
void setup () {
  InitDisplay();
  InitKybd();
  #if defined (serialmonitor)
  pinMode(8, INPUT_PULLUP); // RX0
  Serial.begin(9600);
  int start = millis();
  while ((millis() - start) < 5000) { if (Serial) break; }
  #endif
  initworkspace();
  initenv();
  initsleep();
  pfstring(PSTR("uLisp 4.6 "), pserial); pln(pserial);
}

// Read/Evaluate/Print loop

void repl (object *env) {
  for (;;) {
    RandomSeed = micros();
    gc(NULL, env);
    #if defined(printfreespace)
    pint(Freespace, pserial);
    #endif
    if (BreakLevel) {
      pfstring(PSTR(" : "), pserial);
      pint(BreakLevel, pserial);
    }
    pserial('>'); pserial(' ');
    Context = NIL;
    object *line = read(gserial);
    if (BreakLevel && line == nil) { pln(pserial); return; }
    if (line == (object *)KET) error2(PSTR("unmatched right bracket"));
    protect(line);
    pfl(pserial);
    line = eval(line, env);
    pfl(pserial);
    printobject(line, pserial);
    unprotect();
    pfl(pserial);
    pln(pserial);
  }
}

void loop () {
  if (!setjmp(toplevel_handler)) {
    #if defined(resetautorun)
    volatile int autorun = 12; // Fudge to keep code size the same
    #else
    volatile int autorun = 13;
    #endif
    if (autorun == 12) autorunimage();
  }
  ulisperror();
  repl(NULL);
}

void ulisperror () {
  // Come here after error
  #if defined (serialmonitor)
  delay(100); while (Serial.available()) Serial.read();
  #endif
  clrflag(NOESC); BreakLevel = 0;
  for (int i=0; i<TRACEMAX; i++) TraceDepth[i] = 0;
  #if defined(sdcardsupport)
  SDpfile.close(); SDgfile.close();
  #endif
  #if defined(lisplibrary)
  if (!tstflag(LIBRARYLOADED)) { setflag(LIBRARYLOADED); loadfromlibrary(NULL); clrflag(NOECHO); }
  #endif
}
