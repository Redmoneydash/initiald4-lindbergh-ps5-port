#include <string.h>
const unsigned char *glGetString(unsigned name);
const unsigned char *glGetString(unsigned name) { (void)name; static unsigned char e[]=""; return e; }
