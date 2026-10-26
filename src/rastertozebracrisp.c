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
 * With ZEBRA_CRISP_PREVIEW=1 in the environment it writes a PDF of the
 * thresholded pages to stdout instead, so users can see exactly what will
 * print (used by the "Preview Zebra Output" PDF Service).
 *
 * Usage (from cupsd): rastertozebracrisp job user title copies options [file]
 */

#include <cups/cups.h>
#include <cups/ppd.h>
#include <cups/raster.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define RASTERTOLABEL "/usr/libexec/cups/filter/rastertolabel"

/* Converts one input row to 1-bit (1 = black) using the threshold. */
static void threshold_row(const cups_page_header2_t *header, const unsigned char *inrow,
                          unsigned char *outrow, unsigned outbpl, int threshold)
{
  unsigned bpp = header->cupsBitsPerPixel;

  if (bpp == 1) {
    /* Already 1-bit (old PPD settings); pass through unchanged. */
    memcpy(outrow, inrow, outbpl);
    return;
  }
  memset(outrow, 0, outbpl);
  for (unsigned x = 0; x < header->cupsWidth; x++) {
    int k;
    if (bpp == 8 && header->cupsColorSpace == CUPS_CSPACE_W)
      k = 255 - inrow[x];          /* white-based: 0 = black */
    else if (bpp == 8)
      k = inrow[x];                /* K: 255 = black */
    else
      k = inrow[x * (bpp / 8)];    /* 16-bit: use high byte */
    if (k >= threshold)
      outrow[x >> 3] |= 0x80 >> (x & 7);
  }
}

/* Preview mode: thresholded pages as a PDF of 1-bit images on stdout. */

typedef struct {
  unsigned width, height, bpl;
  double   points_w, points_h;
  unsigned char *bits;
} preview_page_t;

static long pdf_offset;

static void pdf_printf(const char *format, ...)
{
  va_list ap;
  va_start(ap, format);
  pdf_offset += vprintf(format, ap);
  va_end(ap);
}

static int write_preview(cups_raster_t *in, int threshold)
{
  preview_page_t *pages = NULL;
  int num_pages = 0;
  cups_page_header2_t header;

  while (cupsRasterReadHeader2(in, &header)) {
    pages = realloc(pages, (num_pages + 1) * sizeof(*pages));
    preview_page_t *pg = &pages[num_pages++];
    pg->width    = header.cupsWidth;
    pg->height   = header.cupsHeight;
    pg->bpl      = (header.cupsWidth + 7) / 8;
    pg->points_w = header.cupsWidth * 72.0 / (header.HWResolution[0] ? header.HWResolution[0] : 203);
    pg->points_h = header.cupsHeight * 72.0 / (header.HWResolution[1] ? header.HWResolution[1] : 203);
    pg->bits     = calloc(pg->height, pg->bpl);

    unsigned char *inrow = malloc(header.cupsBytesPerLine);
    for (unsigned y = 0; y < header.cupsHeight; y++) {
      if (cupsRasterReadPixels(in, inrow, header.cupsBytesPerLine) < 1)
        break;
      threshold_row(&header, inrow, pg->bits + y * pg->bpl, pg->bpl, threshold);
    }
    free(inrow);
  }
  if (!num_pages) {
    fputs("ERROR: No pages found\n", stderr);
    return 1;
  }

  /* Objects: 1 catalog, 2 pages, then page/contents/image per page. */
  long *offsets = calloc(3 + 3 * num_pages, sizeof(long));
  pdf_printf("%%PDF-1.4\n");
  offsets[1] = pdf_offset;
  pdf_printf("1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n");
  offsets[2] = pdf_offset;
  pdf_printf("2 0 obj\n<< /Type /Pages /Count %d /Kids [", num_pages);
  for (int i = 0; i < num_pages; i++)
    pdf_printf(" %d 0 R", 3 + 3 * i);
  pdf_printf(" ] >>\nendobj\n");

  for (int i = 0; i < num_pages; i++) {
    preview_page_t *pg = &pages[i];
    int obj = 3 + 3 * i;
    char contents[128];
    int clen = snprintf(contents, sizeof(contents), "q %.3f 0 0 %.3f 0 0 cm /Im0 Do Q\n",
                        pg->points_w, pg->points_h);

    offsets[obj] = pdf_offset;
    pdf_printf("%d 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 %.3f %.3f] "
               "/Resources << /XObject << /Im0 %d 0 R >> >> /Contents %d 0 R >>\nendobj\n",
               obj, pg->points_w, pg->points_h, obj + 2, obj + 1);
    offsets[obj + 1] = pdf_offset;
    pdf_printf("%d 0 obj\n<< /Length %d >>\nstream\n%sendstream\nendobj\n", obj + 1, clen, contents);
    offsets[obj + 2] = pdf_offset;
    /* Set bits are black, so invert DeviceGray with /Decode [1 0]. */
    pdf_printf("%d 0 obj\n<< /Type /XObject /Subtype /Image /Width %u /Height %u "
               "/ColorSpace /DeviceGray /BitsPerComponent 1 /Decode [1 0] /Interpolate false "
               "/Length %u >>\nstream\n", obj + 2, pg->width, pg->height, pg->bpl * pg->height);
    pdf_offset += fwrite(pg->bits, 1, (size_t)pg->bpl * pg->height, stdout);
    pdf_printf("\nendstream\nendobj\n");
    free(pg->bits);
  }

  long xref = pdf_offset;
  int num_objs = 3 + 3 * num_pages;
  pdf_printf("xref\n0 %d\n0000000000 65535 f \n", num_objs);
  for (int i = 1; i < num_objs; i++)
    pdf_printf("%010ld 00000 n \n", offsets[i]);
  pdf_printf("trailer\n<< /Size %d /Root 1 0 R >>\nstartxref\n%ld\n%%%%EOF\n", num_objs, xref);

  free(offsets);
  free(pages);
  return 0;
}

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

  /* Threshold: K values >= this become black (K: 0 = white, 255 = black).
   * Normal prints anything darker than 25% gray as black; the "Line Weight"
   * PPD option moves it so thin strokes print lighter or heavier.  macOS
   * color-corrects grays before they reach us (25% gray in the document
   * arrives as K=78), so the cutoffs are measured, not simply 25% of 255. */
  int threshold = 79;                   /* darker than 25% gray */
  ppd_file_t *ppd = ppdOpenFile(getenv("PPD"));
  if (ppd) {
    cups_option_t *options = NULL;
    int num_options = cupsParseOptions(argv[5], 0, &options);
    ppdMarkDefaults(ppd);
    cupsMarkOptions(ppd, num_options, options);
    ppd_choice_t *weight = ppdFindMarkedChoice(ppd, "zeCrispLineWeight");
    if (weight && !strcmp(weight->choice, "Thin"))
      threshold = 147;                  /* darker than 50% gray */
    else if (weight && !strcmp(weight->choice, "Bold"))
      threshold = 41;                   /* darker than 12.5% gray */
    cupsFreeOptions(num_options, options);
    ppdClose(ppd);
  }

  if (getenv("ZEBRA_CRISP_PREVIEW")) {
    cups_raster_t *in = cupsRasterOpen(in_fd, CUPS_RASTER_READ);
    int status = write_preview(in, threshold);
    cupsRasterClose(in);
    return status;
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
      threshold_row(&header, inrow, outrow, outbpl, threshold);
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
