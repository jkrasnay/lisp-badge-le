// Definitions

// Raw Keyboard Handling
//
// By default, LispBadgeLE.ino buffers a line and evals it when you
// press Enter.  To build interactive functions (apps?) we need to be
// able to read keystrokes directly.
//
// LispBadgeLE provides the `keyboard` to disable keyboard scanning
// entirely and `check-key` to check if a particular key is currently
// being pressed but that's not exactly what we want.  The keyboard
// buffering is nice, but we would like to avoid the interactive
// aspects of it such as paren highlighting and just have it buffer
// char presses that we can later retrieve.
//
// Here we implement the `read-char` function, which reads the next
// char from the keyboard buffer.  So this does not interfere with
// with the default keyboard handling, we've implemented a "raw"
// mode in LispBadgeLE.  When in this raw mode, keyboard scanning
// is still enabled and the keyboard buffer is still used, but no
// other handling takes place.
//
// You enter raw mode by using the `with-kb-raw` special form. Raw
// mode is only enabled while evaluating the body of `with-kb-raw`,
// and thus this is the only valid place where you can call
// `read-char`.  Once the evaluation of `with-kb-raw` completes
// then keyboard handling reverts to the default.
//
const char notrawmode[] PROGMEM = "must be called in raw mode, see with-kb-raw";

object *fn_readchar(object *args, object *env) {
  (void) env;
  if (KybdRawMode) {
    int nargs = listlength(args);
    unsigned long max_millis = 0;
    if (nargs > 0) {
      max_millis = checkinteger(first(args));
    }
    unsigned long start = millis();
    while (nargs == 0 || millis() - start < max_millis) {
      char c = GetRawKey();
      if (c) {
        return character(c);
      }
    }
    return nil;
  } else {
    error2(notrawmode);
    return nil;
  }
}


object *sp_withkbraw(object *args, object *env) {
  (void) env;
  if (!KybdRawMode) {
    WritePtr = 0;
    ReadPtr = 0;
    LastWritePtr = 0;
  }
  // Allow nesting of with-kb-raw.
  KybdRawMode++;
  object *result = eval(tf_progn(args, env), env);
  KybdRawMode--;
  if (!KybdRawMode) {
    WritePtr = 0;
    ReadPtr = 0;
    LastWritePtr = 0;
  }
  return result;
}


// Character Grid
//
// To implement our visual editor it helps to treat the display as
// a two-dimensional grid of characters.  LispBadgeLE has some
// support for this in the form of PlotChar and ScrollDisplay but
// these are not exposed to the Lisp layer.  Here we provide a
// Lisp-level interface to the character grid abstraction.
//

object *fn_plotchar(object *args, object *env) {
  (void) env;
  uint8_t ch = checkchar(first(args));
  uint8_t line = checkinteger(second(args));
  uint8_t col = checkinteger(third(args));
  int nargs = listlength(args);
  if (nargs > 3 && car(cdr(cdr(cdr(args))))) {
    ch |= 0x80;
  }
  // TODO this means we can't use chars > 0x7f.  We might want
  // to implement our own PlotChar to fix this.
  PlotChar(ch, line, col);
  return first(args);
}


//--- Symbol names --------------------------------------------------


const char stringreadchar[] PROGMEM = "read-char";
const char stringwithkbraw[] PROGMEM = "with-kb-raw";
const char stringplotchar[] PROGMEM = "plot-char";


//--- Documentation strings --------------------------------------------------


// Raw Keyboard Handling
//                                        |
const char docreadchar[] PROGMEM = "(read-char [max-millis])\n"
"Reads the next character from the\n"
"keyboard buffer, or nil if no character\n"
"is available.  Waits for at most\n"
"`max-millis`.  Call with no args to wait"
"forever.  Must be called from within\n"
"`with-kb-raw`.";
//                                        |
const char docwithkbraw[] PROGMEM = "(with-kb-raw form*)\n"
"Evaluates several forms with the keyboard\n"
"in raw mode.  Forms may contain calls to\n"
"`read-char`.";
//                                        |

// Character Grid
//                                        |
const char docplotchar[] PROGMEM = "(plot-char char line col invert)\n"
"Draws `char` at the given line/column.\n"
"Inverts the display if `invert` is `t`.\n";


// Symbol lookup table
const tbl_entry_t lookup_table2[] PROGMEM = {
  { stringreadchar, fn_readchar, 0201, docreadchar },
  { stringwithkbraw, sp_withkbraw, 0307, docwithkbraw },
  { stringplotchar, fn_plotchar, 0234, docplotchar },
};


// Table cross-reference functions

tbl_entry_t *tables[] = {lookup_table, lookup_table2};
const int tablesizes[] = { arraysize(lookup_table), arraysize(lookup_table2) };

const tbl_entry_t *table (int n) {
  return tables[n];
}

int tablesize (int n) {
  return tablesizes[n];
}
