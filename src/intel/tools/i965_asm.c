/*
 * Copyright © 2018 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 */

#include <stdio.h>
#include <getopt.h>
#include "i965_asm.h"

extern FILE *yyin;
struct brw_codegen *p;
static int c_literal_output = 0;
char *input_filename = NULL;
int errors;

static void
print_help(const char *progname, FILE *file)
{
   fprintf(file,
           "Usage: %s [OPTION] inputfile\n"
           "Assemble i965 instructions from input file.\n\n"
           "    -h, --help             display this help and exit\n"
           "    -l, --c-literal        C literal\n"
           "    -o, --output           specify output file\n"
           "        --compact          print compacted instructions\n"
           "    -g, --gen=platform     assemble instructions for given \n"
           "                           platform (3 letter platform name)\n"
           "Example:\n"
           "    i965_asm -g kbl input.asm -o output\n",
           progname);
}

static void
print_instruction(FILE *output, bool compact, const brw_inst *instruction)
{
   int byte_limit;

   byte_limit = (compact == true) ? 8 : 16;

   if (c_literal_output) {
      fprintf(output, "\t0x%02x,", ((unsigned char *)instruction)[0]);

      for (unsigned i = 1; i < byte_limit; i++)
         fprintf(output, " 0x%02x,", ((unsigned char *)instruction)[i]);
   } else {
      fprintf(output, "%02x", ((unsigned char *)instruction)[0]);

      for (unsigned i = 1; i < byte_limit; i++)
         fprintf(output, " %02x", ((unsigned char *)instruction)[i]);
   }
   fprintf(output, "\n");
}

static struct gen_device_info *
i965_disasm_init(uint16_t pci_id)
{
   struct gen_device_info *devinfo;

   devinfo = malloc(sizeof *devinfo);
   if (devinfo == NULL)
      return NULL;

   if (!gen_get_device_info_from_pci_id(pci_id, devinfo)) {
      fprintf(stderr, "can't find device information: pci_id=0x%x\n",
              pci_id);
      free(devinfo);
      return NULL;
   }

   brw_init_compaction_tables(devinfo);

   return devinfo;
}

int main(int argc, char **argv)
{
   char *output_file = NULL;
   char c;
   FILE *output = stdout;
   bool help = false, compact = false;
   void *store;
   uint64_t pci_id = 0;
   int offset = 0, err;
   int start_offset = 0;
   struct disasm_info *disasm_info;
   struct gen_device_info *devinfo = NULL;
   int result = EXIT_FAILURE;

   const struct option i965_asm_opts[] = {
      { "help",          no_argument,       (int *) &help,      true },
      { "c-literal",     no_argument,       NULL,               'c' },
      { "gen",           required_argument, NULL,               'g' },
      { "output",        required_argument, NULL,               'o' },
      { "compact",       no_argument,       (int *) &compact,   true },
      { NULL,            0,                 NULL,               0 }
   };

   while ((c = getopt_long(argc, argv, ":g:o:lh", i965_asm_opts, NULL)) != -1) {
      switch (c) {
      case 'g': {
         const int id = gen_device_name_to_pci_device_id(optarg);
         if (id < 0) {
            fprintf(stderr, "can't parse gen: '%s', expected 3 letter "
                            "platform name\n", optarg);
            goto end;
         } else {
            pci_id = id;
         }
         break;
      }
      case 'h':
         help = true;
         print_help(argv[0], stderr);
         goto end;
      case 'l':
         c_literal_output = 1;
         break;
      case 'o':
         output_file = strdup(optarg);
         break;
      case 0:
         break;
      case ':':
         fprintf(stderr, "%s: option `-%c' requires an argument\n",
                 argv[0], optopt);
         goto end;
      case '?':
      default:
         fprintf(stderr, "%s: option `-%c' is invalid: ignored\n",
                 argv[0], optopt);
         goto end;
      }
   }

   if (help || !pci_id) {
      print_help(argv[0], stderr);
      goto end;
   }

   if (!argv[optind]) {
      fprintf(stderr, "Please specify input file\n");
      goto end;
   }

   input_filename = strdup(argv[optind]);
   yyin = fopen(input_filename, "r");
   if (!yyin) {
      fprintf(stderr, "Unable to read input file : %s\n",
              input_filename);
      goto end;
   }

   if (output_file) {
      output = fopen(output_file, "w");
      if (!output) {
         fprintf(stderr, "Couldn't open output file\n");
         goto end;
      }
   }

   devinfo = i965_disasm_init(pci_id);
   if (!devinfo) {
      fprintf(stderr, "Unable to allocate memory for "
                      "gen_device_info struct instance.\n");
      goto end;
   }

   p = rzalloc(NULL, struct brw_codegen);
   brw_init_codegen(devinfo, p, p);
   p->automatic_exec_sizes = false;

   err = yyparse();
   if (err || errors)
      goto end;

   store = p->store;

   disasm_info = disasm_initialize(p->devinfo, NULL);
   if (!disasm_info) {
      fprintf(stderr, "Unable to initialize disasm_info struct instance\n");
      goto end;
   }

   if (c_literal_output)
      fprintf(output, "static const char gen_eu_bytes[] = {\n");

   brw_validate_instructions(p->devinfo, p->store, 0,
                             p->next_insn_offset, disasm_info);

   const int nr_insn = (p->next_insn_offset - start_offset) / 16;

   if (compact)
      brw_compact_instructions(p, start_offset, disasm_info);

   for (int i = 0; i < nr_insn; i++) {
      const brw_inst *insn = store + offset;
      bool compacted = false;

      if (compact && brw_inst_cmpt_control(p->devinfo, insn)) {
            offset += 8;
            compacted = true;
      } else {
            offset += 16;
      }

      print_instruction(output, compacted, insn);
   }

   ralloc_free(disasm_info);

   if (c_literal_output)
      fprintf(output, "}");

   result = EXIT_SUCCESS;
   goto end;

end:
   free(input_filename);
   free(output_file);

   if (yyin)
      fclose(yyin);

   if (output)
      fclose(output);

   if (p)
      ralloc_free(p);

   if (devinfo)
      free(devinfo);

   exit(result);
}
