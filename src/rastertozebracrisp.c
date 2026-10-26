/*
 * rastertozebracrisp - CUPS filter for Zebra ZPL label printers.
 *
 * Copyright 2026 Johnny Lee
 * Licensed under the Apache License, Version 2.0 (see LICENSE).
 *
 * Reads 8-bit grayscale (cupsColorSpace K) CUPS raster, thresholds it to
 * pure black/white with no dithering, and pipes the 1-bit result into the
 * stock rastertolabel filter so every existing driver option still applies.
 *
 * Usage (from cupsd): rastertozebracrisp job user title copies options [file]
 */

#include <cups/cups.h>
#include <cups/ppd.h>
#include <cups/raster.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define RASTERTOLABEL "/usr/libexec/cups/filter/rastertolabel"

int main(int argc, char *argv[])
{
  if (argc < 6 || argc > 7) {
    fprintf(stderr, "Usage: %s job user title copies options [file]\n", argv[0]);
    return 1;
  }

  int in_fd = 0;
  if (argc == 7 && (in_fd = open(argv[6], O_RDONLY)) < 0) {
    fprintf(stderr, "ERROR: Unable to open %s: %s\n", argv[6], strerror(errno));
    return 1;
  }

  /* Threshold: K values >= this become black.  The "Line Weight" PPD
   * option moves it so thin strokes print lighter or heavier. */
  int threshold = 128;
  ppd_file_t *ppd = ppdOpenFile(getenv("PPD"));
  if (ppd) {
    cups_option_t *options = NULL;
    int num_options = cupsParseOptions(argv[5], 0, &options);
    ppdMarkDefaults(ppd);
    cupsMarkOptions(ppd, num_options, options);
    ppd_choice_t *weight = ppdFindMarkedChoice(ppd, "zeCrispLineWeight");
    if (weight && !strcmp(weight->choice, "Thin"))
      threshold = 192;
    else if (weight && !strcmp(weight->choice, "Bold"))
      threshold = 64;
    cupsFreeOptions(num_options, options);
    ppdClose(ppd);
  }

  int pipefd[2];
  if (pipe(pipefd)) {
    perror("ERROR: pipe");
    return 1;
  }

  pid_t child = fork();
  if (child < 0) {
    perror("ERROR: fork");
    return 1;
  }
  if (child == 0) {
    /* rastertolabel reads our 1-bit raster on stdin, writes ZPL to stdout. */
    dup2(pipefd[0], 0);
    close(pipefd[0]);
    close(pipefd[1]);
    execl(RASTERTOLABEL, argv[0], argv[1], argv[2], argv[3], argv[4], argv[5], (char *)NULL);
    fprintf(stderr, "ERROR: Unable to run %s: %s\n", RASTERTOLABEL, strerror(errno));
    _exit(1);
  }
  close(pipefd[0]);

  cups_raster_t *in  = cupsRasterOpen(in_fd, CUPS_RASTER_READ);
  cups_raster_t *out = cupsRasterOpen(pipefd[1], CUPS_RASTER_WRITE);
  cups_page_header2_t header;
  int page = 0;

  while (cupsRasterReadHeader2(in, &header)) {
    page++;
    fprintf(stderr, "INFO: Crisp-converting page %d (%ux%u, %u bpp)\n", page,
            header.cupsWidth, header.cupsHeight, header.cupsBitsPerPixel);

    unsigned inbpl = header.cupsBytesPerLine;
    unsigned bpp   = header.cupsBitsPerPixel;
    unsigned char *inrow  = malloc(inbpl);
    unsigned outbpl = (header.cupsWidth + 7) / 8;
    unsigned char *outrow = malloc(outbpl);

    cups_page_header2_t outheader = header;
    outheader.cupsBitsPerColor = 1;
    outheader.cupsBitsPerPixel = 1;
    outheader.cupsBytesPerLine = outbpl;
    outheader.cupsColorSpace   = CUPS_CSPACE_K;
    cupsRasterWriteHeader2(out, &outheader);

    for (unsigned y = 0; y < header.cupsHeight; y++) {
      if (cupsRasterReadPixels(in, inrow, inbpl) < 1)
        break;
      if (bpp == 1) {
        /* Already 1-bit (old PPD settings); pass through unchanged. */
        memcpy(outrow, inrow, outbpl);
      } else {
        memset(outrow, 0, outbpl);
        for (unsigned x = 0; x < header.cupsWidth; x++) {
          int k;
          if (bpp == 8 && header.cupsColorSpace == CUPS_CSPACE_W)
            k = 255 - inrow[x];          /* white-based: 0 = black */
          else if (bpp == 8)
            k = inrow[x];                /* K: 255 = black */
          else
            k = inrow[x * (bpp / 8)];    /* 16-bit: use high byte */
          if (k >= threshold)
            outrow[x >> 3] |= 0x80 >> (x & 7);
        }
      }
      cupsRasterWritePixels(out, outrow, outbpl);
    }
    free(inrow);
    free(outrow);
  }

  cupsRasterClose(in);
  cupsRasterClose(out);
  close(pipefd[1]);
  if (in_fd)
    close(in_fd);

  int status = 0;
  waitpid(child, &status, 0);
  if (page == 0)
    fputs("ERROR: No pages found\n", stderr);
  return (page && WIFEXITED(status) && WEXITSTATUS(status) == 0) ? 0 : 1;
}
