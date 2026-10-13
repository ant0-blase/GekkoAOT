/* v328: augment GNU ld's default script. Profile-guided machine function
   splitting emits the cold part of each function as .text.split.<name>; the
   default .text rule would interleave those parts with hot code again. */
SECTIONS
{
  .text.split : { *(.text.split .text.split.*) }
}
INSERT BEFORE .text;
