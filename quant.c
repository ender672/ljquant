/*
 * jquant2.c
 *
 * This file was part of the Independent JPEG Group's software:
 * Copyright (C) 1991-1996, Thomas G. Lane.
 * libjpeg-turbo Modifications:
 * Copyright (C) 2009, 2014, D. R. Commander.
 * For conditions of distribution and use, see the accompanying LICENSE file.
 *
 * This file contains 2-pass color quantization (color mapping) routines.
 * These routines provide selection of a custom color map for an image,
 * followed by mapping of the image to that color map, with optional
 * Floyd-Steinberg dithering.
 * It is also possible to use just the second pass to map to an arbitrary
 * externally-given color map.
 *
 * Note: ordered dithering is not supported, since there isn't any fast
 * way to compute intercolor distances; it's unclear that ordered dither's
 * fundamental assumptions even hold with an irregularly spaced color map.
 */

#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include "quant.h"

#define RIGHT_SHIFT(x,shft)     ((x) >> (shft))
#define MAXJSAMPLE      255
#define BITS_IN_JSAMPLE 8

/*
 * This module implements the well-known Heckbert paradigm for color
 * quantization.  Most of the ideas used here can be traced back to
 * Heckbert's seminal paper
 *   Heckbert, Paul.  "Color Image Quantization for Frame Buffer Display",
 *   Proc. SIGGRAPH '82, Computer Graphics v.16 #3 (July 1982), pp 297-304.
 *
 * In the first pass over the image, we accumulate a histogram showing the
 * usage count of each possible color.  To keep the histogram to a reasonable
 * size, we reduce the precision of the input; typical practice is to retain
 * 5 or 6 bits per color, so that 8 or 4 different input values are counted
 * in the same histogram cell.
 *
 * Next, the color-selection step begins with a box representing the whole
 * color space, and repeatedly splits the "largest" remaining box until we
 * have as many boxes as desired colors.  Then the mean color in each
 * remaining box becomes one of the possible output colors.
 *
 * The second pass over the image maps each input pixel to the closest output
 * color (optionally after applying a Floyd-Steinberg dithering correction).
 * This mapping is logically trivial, but making it go fast enough requires
 * considerable care.
 *
 * Heckbert-style quantizers vary a good deal in their policies for choosing
 * the "largest" box and deciding where to cut it.  The particular policies
 * used here have proved out well in experimental comparisons, but better ones
 * may yet be found.
 *
 * In earlier versions of the IJG code, this module quantized in YCbCr color
 * space, processing the raw upsampled data without a color conversion step.
 * This allowed the color conversion math to be done only once per colormap
 * entry, not once per pixel.  However, that optimization precluded other
 * useful optimizations (such as merging color conversion with upsampling)
 * and it also interfered with desired capabilities such as quantizing to an
 * externally-supplied colormap.  We have therefore abandoned that approach.
 * The present code works in the post-conversion color space, typically RGB.
 *
 * To improve the visual quality of the results, we actually work in scaled
 * RGB space, giving G distances more weight than R, and R in turn more than
 * B.  To do everything in integer math, we must use integer scale factors.
 * The 2/3/1 scale factors used here correspond loosely to the relative
 * weights of the colors in the NTSC grayscale equation.
 * If you want to use this code to quantize a non-RGB color space, you'll
 * probably need to change these scale factors.
 */

#define R_SCALE 2               /* scale R distances by this much */
#define G_SCALE 3               /* scale G distances by this much */
#define B_SCALE 1               /* and B by this much */

static const int c_scales[3]={R_SCALE, G_SCALE, B_SCALE};
#define C0_SCALE c_scales[0]
#define C1_SCALE c_scales[1]
#define C2_SCALE c_scales[2]

/*
 * First we have the histogram data structure and routines for creating it.
 *
 * The number of bits of precision can be adjusted by changing these symbols.
 * We recommend keeping 6 bits for G and 5 each for R and B.
 * If you have plenty of memory and cycles, 6 bits all around gives marginally
 * better results; if you are short of memory, 5 bits all around will save
 * some space but degrade the results.
 * To maintain a fully accurate histogram, we'd need to allocate a "long"
 * (preferably unsigned long) for each cell.  In practice this is overkill;
 * we can get by with 16 bits per cell.  Few of the cell counts will overflow,
 * and clamping those that do overflow to the maximum value will give close-
 * enough results.  This reduces the recommended histogram size from 256Kb
 * to 128Kb, which is a useful savings on PC-class machines.
 * (In the second pass the histogram space is re-used for pixel mapping data;
 * in that capacity, each cell must be able to store zero to the number of
 * desired colors.  16 bits/cell is plenty for that too.)
 * Since the JPEG code is intended to run in small memory model on 80x86
 * machines, we can't just allocate the histogram in one chunk.  Instead
 * of a true 3-D array, we use a row of pointers to 2-D arrays.  Each
 * pointer corresponds to a C0 value (typically 2^5 = 32 pointers) and
 * each 2-D array has 2^6*2^5 = 2048 or 2^6*2^6 = 4096 entries.
 */

#define MAXNUMCOLORS  (MAXJSAMPLE+1) /* maximum size of colormap */

/* These will do the right thing for either R,G,B or B,G,R color order,
 * but you may not like the results for other color orders.
 */
#define HIST_C0_BITS  5         /* bits of precision in R/B histogram */
#define HIST_C1_BITS  6         /* bits of precision in G histogram */

/* Number of elements along histogram axes. */
#define HIST_C0_ELEMS  (1<<HIST_C0_BITS)
#define HIST_C1_ELEMS  (1<<HIST_C1_BITS)

/* These are the amounts to shift an input value to get a histogram index. */
#define C0_SHIFT  (BITS_IN_JSAMPLE-HIST_C0_BITS)
#define C1_SHIFT  (BITS_IN_JSAMPLE-HIST_C1_BITS)
#define C2_SHIFT  (BITS_IN_JSAMPLE-HIST_C2_BITS)

/* Declarations for Floyd-Steinberg dithering.
 *
 * Errors are accumulated into the array fserrors[], at a resolution of
 * 1/16th of a pixel count.  The error at a given pixel is propagated
 * to its not-yet-processed neighbors using the standard F-S fractions,
 *              ...     (here)  7/16
 *              3/16    5/16    1/16
 * We work left-to-right on even rows, right-to-left on odd rows.
 *
 * We can get away with a single array (holding one row's worth of errors)
 * by using it to store the current row's errors at pixel columns not yet
 * processed, but the next row's errors at columns already processed.  We
 * need only a few extra variables to hold the errors immediately around the
 * current column.  (If we are lucky, those variables are in registers, but
 * even if not, they're probably cheaper to access than array elements are.)
 *
 * The fserrors[] array has (#columns + 2) entries; the extra entry at
 * each end saves us from special-casing the first and last pixels.
 * Each entry is three values long, one value for each color component.
 */

typedef int LOCFSERROR;         /* use 'int' for calculation temps */
typedef histcell * histptr; /* for pointers to histogram cells */


/*
 * Prescan some rows of pixels.
 * In this module the prescan simply updates the histogram, which has been
 * initialized to zeroes by start_pass.
 * An output_buf parameter is required by the method signature, but no data
 * is actually output (in fact the buffer controller is probably passing a
 * NULL pointer).
 */

void
quant_index (struct quant *cquantize, unsigned char *ptr)
{
  register histptr histp;
  register hist3d histogram = cquantize->histogram;
  unsigned int col;
  unsigned int width = cquantize->output_width;

  for (col = width; col > 0; col--) {
    /* get pixel value and index into the histogram */
    histp = & histogram[ptr[0] >> C0_SHIFT]
                       [ptr[1] >> C1_SHIFT]
                       [ptr[2] >> C2_SHIFT];
    /* increment, check for overflow and undo increment if so. */
    if (++(*histp) <= 0)
      (*histp)--;
    ptr += 3;
  }
}


/*
 * Next we have the really interesting routines: selection of a colormap
 * given the completed histogram.
 * These routines work with a list of "boxes", each representing a rectangular
 * subset of the input color space (to histogram precision).
 */

typedef struct {
  /* The bounds of the box (inclusive); expressed as histogram indexes */
  int c0min, c0max;
  int c1min, c1max;
  int c2min, c2max;
  /* The volume (actually 2-norm) of the box */
  int32_t volume;
  /* The number of nonzero histogram cells within this box */
  long colorcount;
} box;

typedef box * boxptr;


static boxptr
find_biggest_color_pop (boxptr boxlist, int numboxes)
/* Find the splittable box with the largest color population */
/* Returns NULL if no splittable boxes remain */
{
  register boxptr boxp;
  register int i;
  register long maxc = 0;
  boxptr which = NULL;

  for (i = 0, boxp = boxlist; i < numboxes; i++, boxp++) {
    if (boxp->colorcount > maxc && boxp->volume > 0) {
      which = boxp;
      maxc = boxp->colorcount;
    }
  }
  return which;
}


static boxptr
find_biggest_volume (boxptr boxlist, int numboxes)
/* Find the splittable box with the largest (scaled) volume */
/* Returns NULL if no splittable boxes remain */
{
  register boxptr boxp;
  register int i;
  register int32_t maxv = 0;
  boxptr which = NULL;

  for (i = 0, boxp = boxlist; i < numboxes; i++, boxp++) {
    if (boxp->volume > maxv) {
      which = boxp;
      maxv = boxp->volume;
    }
  }
  return which;
}


static void
update_box (struct quant *cquantize, boxptr boxp)
/* Shrink the min/max bounds of a box to enclose only nonzero elements, */
/* and recompute its volume and population */
{
  hist3d histogram = cquantize->histogram;
  histptr histp;
  int c0,c1,c2;
  int c0min,c0max,c1min,c1max,c2min,c2max;
  int32_t dist0,dist1,dist2;
  long ccount;

  c0min = boxp->c0min;  c0max = boxp->c0max;
  c1min = boxp->c1min;  c1max = boxp->c1max;
  c2min = boxp->c2min;  c2max = boxp->c2max;

  if (c0max > c0min)
    for (c0 = c0min; c0 <= c0max; c0++)
      for (c1 = c1min; c1 <= c1max; c1++) {
        histp = & histogram[c0][c1][c2min];
        for (c2 = c2min; c2 <= c2max; c2++)
          if (*histp++ != 0) {
            boxp->c0min = c0min = c0;
            goto have_c0min;
          }
      }
 have_c0min:
  if (c0max > c0min)
    for (c0 = c0max; c0 >= c0min; c0--)
      for (c1 = c1min; c1 <= c1max; c1++) {
        histp = & histogram[c0][c1][c2min];
        for (c2 = c2min; c2 <= c2max; c2++)
          if (*histp++ != 0) {
            boxp->c0max = c0max = c0;
            goto have_c0max;
          }
      }
 have_c0max:
  if (c1max > c1min)
    for (c1 = c1min; c1 <= c1max; c1++)
      for (c0 = c0min; c0 <= c0max; c0++) {
        histp = & histogram[c0][c1][c2min];
        for (c2 = c2min; c2 <= c2max; c2++)
          if (*histp++ != 0) {
            boxp->c1min = c1min = c1;
            goto have_c1min;
          }
      }
 have_c1min:
  if (c1max > c1min)
    for (c1 = c1max; c1 >= c1min; c1--)
      for (c0 = c0min; c0 <= c0max; c0++) {
        histp = & histogram[c0][c1][c2min];
        for (c2 = c2min; c2 <= c2max; c2++)
          if (*histp++ != 0) {
            boxp->c1max = c1max = c1;
            goto have_c1max;
          }
      }
 have_c1max:
  if (c2max > c2min)
    for (c2 = c2min; c2 <= c2max; c2++)
      for (c0 = c0min; c0 <= c0max; c0++) {
        histp = & histogram[c0][c1min][c2];
        for (c1 = c1min; c1 <= c1max; c1++, histp += HIST_C2_ELEMS)
          if (*histp != 0) {
            boxp->c2min = c2min = c2;
            goto have_c2min;
          }
      }
 have_c2min:
  if (c2max > c2min)
    for (c2 = c2max; c2 >= c2min; c2--)
      for (c0 = c0min; c0 <= c0max; c0++) {
        histp = & histogram[c0][c1min][c2];
        for (c1 = c1min; c1 <= c1max; c1++, histp += HIST_C2_ELEMS)
          if (*histp != 0) {
            boxp->c2max = c2max = c2;
            goto have_c2max;
          }
      }
 have_c2max:

  /* Update box volume.
   * We use 2-norm rather than real volume here; this biases the method
   * against making long narrow boxes, and it has the side benefit that
   * a box is splittable iff norm > 0.
   * Since the differences are expressed in histogram-cell units,
   * we have to shift back to unsigned char units to get consistent distances;
   * after which, we scale according to the selected distance scale factors.
   */
  dist0 = ((c0max - c0min) << C0_SHIFT) * C0_SCALE;
  dist1 = ((c1max - c1min) << C1_SHIFT) * C1_SCALE;
  dist2 = ((c2max - c2min) << C2_SHIFT) * C2_SCALE;
  boxp->volume = dist0*dist0 + dist1*dist1 + dist2*dist2;

  /* Now scan remaining volume of box and compute population */
  ccount = 0;
  for (c0 = c0min; c0 <= c0max; c0++)
    for (c1 = c1min; c1 <= c1max; c1++) {
      histp = & histogram[c0][c1][c2min];
      for (c2 = c2min; c2 <= c2max; c2++, histp++)
        if (*histp != 0) {
          ccount++;
        }
    }
  boxp->colorcount = ccount;
}


static int
median_cut (struct quant *cquantize, boxptr boxlist, int numboxes,
            int desired_colors)
/* Repeatedly select and split the largest box until we have enough boxes */
{
  int n,lb;
  int c0,c1,c2,cmax;
  register boxptr b1,b2;

  while (numboxes < desired_colors) {
    /* Select box to split.
     * Current algorithm: by population for first half, then by volume.
     */
    if (numboxes*2 <= desired_colors) {
      b1 = find_biggest_color_pop(boxlist, numboxes);
    } else {
      b1 = find_biggest_volume(boxlist, numboxes);
    }
    if (b1 == NULL)             /* no splittable boxes left! */
      break;
    b2 = &boxlist[numboxes];    /* where new box will go */
    /* Copy the color bounds to the new box. */
    b2->c0max = b1->c0max; b2->c1max = b1->c1max; b2->c2max = b1->c2max;
    b2->c0min = b1->c0min; b2->c1min = b1->c1min; b2->c2min = b1->c2min;
    /* Choose which axis to split the box on.
     * Current algorithm: longest scaled axis.
     * See notes in update_box about scaling distances.
     */
    c0 = ((b1->c0max - b1->c0min) << C0_SHIFT) * C0_SCALE;
    c1 = ((b1->c1max - b1->c1min) << C1_SHIFT) * C1_SCALE;
    c2 = ((b1->c2max - b1->c2min) << C2_SHIFT) * C2_SCALE;
    /* We want to break any ties in favor of green, then red, blue last.
     * This code does the right thing for R,G,B or B,G,R color orders only.
     */
    cmax = c1; n = 1;
    if (c0 > cmax) { cmax = c0; n = 0; }
    if (c2 > cmax) { n = 2; }
    /* Choose split point along selected axis, and update box bounds.
     * Current algorithm: split at halfway point.
     * (Since the box has been shrunk to minimum volume,
     * any split will produce two nonempty subboxes.)
     * Note that lb value is max for lower box, so must be < old max.
     */
    switch (n) {
    case 0:
      lb = (b1->c0max + b1->c0min) / 2;
      b1->c0max = lb;
      b2->c0min = lb+1;
      break;
    case 1:
      lb = (b1->c1max + b1->c1min) / 2;
      b1->c1max = lb;
      b2->c1min = lb+1;
      break;
    case 2:
      lb = (b1->c2max + b1->c2min) / 2;
      b1->c2max = lb;
      b2->c2min = lb+1;
      break;
    }
    /* Update stats for boxes */
    update_box(cquantize, b1);
    update_box(cquantize, b2);
    numboxes++;
  }
  return numboxes;
}


static void
compute_color (struct quant *cquantize, boxptr boxp, int icolor)
/* Compute representative color for a box, put it in colormap[icolor] */
{
  /* Current algorithm: mean weighted by pixels (not colors) */
  /* Note it is important to get the rounding correct! */
  hist3d histogram = cquantize->histogram;
  histptr histp;
  int c0,c1,c2;
  int c0min,c0max,c1min,c1max,c2min,c2max;
  long count;
  long total = 0;
  long c0total = 0;
  long c1total = 0;
  long c2total = 0;

  c0min = boxp->c0min;  c0max = boxp->c0max;
  c1min = boxp->c1min;  c1max = boxp->c1max;
  c2min = boxp->c2min;  c2max = boxp->c2max;

  for (c0 = c0min; c0 <= c0max; c0++)
    for (c1 = c1min; c1 <= c1max; c1++) {
      histp = & histogram[c0][c1][c2min];
      for (c2 = c2min; c2 <= c2max; c2++) {
        if ((count = *histp++) != 0) {
          total += count;
          c0total += ((c0 << C0_SHIFT) + ((1<<C0_SHIFT)>>1)) * count;
          c1total += ((c1 << C1_SHIFT) + ((1<<C1_SHIFT)>>1)) * count;
          c2total += ((c2 << C2_SHIFT) + ((1<<C2_SHIFT)>>1)) * count;
        }
      }
    }

  cquantize->colormap[0][icolor] = (unsigned char) ((c0total + (total>>1)) / total);
  cquantize->colormap[1][icolor] = (unsigned char) ((c1total + (total>>1)) / total);
  cquantize->colormap[2][icolor] = (unsigned char) ((c2total + (total>>1)) / total);
}


static void
select_colors (struct quant *cquantize, int desired_colors)
/* Master routine for color selection */
{
  boxptr boxlist;
  int numboxes;
  int i;

  /* Allocate workspace for box list. */
  boxlist = malloc(desired_colors * sizeof(box));
  /* Initialize one box containing whole space */
  numboxes = 1;
  boxlist[0].c0min = 0;
  boxlist[0].c0max = MAXJSAMPLE >> C0_SHIFT;
  boxlist[0].c1min = 0;
  boxlist[0].c1max = MAXJSAMPLE >> C1_SHIFT;
  boxlist[0].c2min = 0;
  boxlist[0].c2max = MAXJSAMPLE >> C2_SHIFT;
  /* Shrink it to actually-used volume and set its statistics */
  update_box(cquantize, & boxlist[0]);
  /* Perform median-cut to produce final box list */
  numboxes = median_cut(cquantize, boxlist, numboxes, desired_colors);
  /* Compute the representative color for each box, fill colormap */
  for (i = 0; i < numboxes; i++)
    compute_color(cquantize, & boxlist[i], i);
  cquantize->actual_number_of_colors = numboxes;

  free(boxlist);
}


/*
 * These routines are concerned with the time-critical task of mapping input
 * colors to the nearest color in the selected colormap.
 *
 * We re-use the histogram space as an "inverse color map", essentially a
 * cache for the results of nearest-color searches.  All colors within a
 * histogram cell will be mapped to the same colormap entry, namely the one
 * closest to the cell's center.  This may not be quite the closest entry to
 * the actual input color, but it's almost as good.  A zero in the cache
 * indicates we haven't found the nearest color for that cell yet; the array
 * is cleared to zeroes before starting the mapping pass.  When we find the
 * nearest color for a cell, its colormap index plus one is recorded in the
 * cache for future use.  The pass2 scanning routines call fill_inverse_cmap
 * when they need to use an unfilled entry in the cache.
 *
 * Our method of efficiently finding nearest colors is based on the "locally
 * sorted search" idea described by Heckbert and on the incremental distance
 * calculation described by Spencer W. Thomas in chapter III.1 of Graphics
 * Gems II (James Arvo, ed.  Academic Press, 1991).  Thomas points out that
 * the distances from a given colormap entry to each cell of the histogram can
 * be computed quickly using an incremental method: the differences between
 * distances to adjacent cells themselves differ by a constant.  This allows a
 * fairly fast implementation of the "brute force" approach of computing the
 * distance from every colormap entry to every histogram cell.  Unfortunately,
 * it needs a work array to hold the best-distance-so-far for each histogram
 * cell (because the inner loop has to be over cells, not colormap entries).
 * The work array elements have to be int32_ts, so the work array would need
 * 256Kb at our recommended precision.  This is not feasible in DOS machines.
 *
 * To get around these problems, we apply Thomas' method to compute the
 * nearest colors for only the cells within a small subbox of the histogram.
 * The work array need be only as big as the subbox, so the memory usage
 * problem is solved.  Furthermore, we need not fill subboxes that are never
 * referenced in pass2; many images use only part of the color gamut, so a
 * fair amount of work is saved.  An additional advantage of this
 * approach is that we can apply Heckbert's locality criterion to quickly
 * eliminate colormap entries that are far away from the subbox; typically
 * three-fourths of the colormap entries are rejected by Heckbert's criterion,
 * and we need not compute their distances to individual cells in the subbox.
 * The speed of this approach is heavily influenced by the subbox size: too
 * small means too much overhead, too big loses because Heckbert's criterion
 * can't eliminate as many colormap entries.  Empirically the best subbox
 * size seems to be about 1/512th of the histogram (1/8th in each direction).
 *
 * Thomas' article also describes a refined method which is asymptotically
 * faster than the brute-force method, but it is also far more complex and
 * cannot efficiently be applied to small subboxes.  It is therefore not
 * useful for programs intended to be portable to DOS machines.  On machines
 * with plenty of memory, filling the whole histogram in one shot with Thomas'
 * refined method might be faster than the present code --- but then again,
 * it might not be any faster, and it's certainly more complicated.
 */


/* log2(histogram cells in update box) for each axis; this can be adjusted */
#define BOX_C0_LOG  (HIST_C0_BITS-3)
#define BOX_C1_LOG  (HIST_C1_BITS-3)
#define BOX_C2_LOG  (HIST_C2_BITS-3)

#define BOX_C0_ELEMS  (1<<BOX_C0_LOG) /* # of hist cells in update box */
#define BOX_C1_ELEMS  (1<<BOX_C1_LOG)
#define BOX_C2_ELEMS  (1<<BOX_C2_LOG)

#define BOX_C0_SHIFT  (C0_SHIFT + BOX_C0_LOG)
#define BOX_C1_SHIFT  (C1_SHIFT + BOX_C1_LOG)
#define BOX_C2_SHIFT  (C2_SHIFT + BOX_C2_LOG)


/*
 * The next three routines implement inverse colormap filling.  They could
 * all be folded into one big routine, but splitting them up this way saves
 * some stack space (the mindist[] and bestdist[] arrays need not coexist)
 * and may allow some compilers to produce better code by registerizing more
 * inner-loop variables.
 */

static int
find_nearby_colors (struct quant *cquantize, int minc0, int minc1, int minc2,
                    unsigned char colorlist[])
/* Locate the colormap entries close enough to an update box to be candidates
 * for the nearest entry to some cell(s) in the update box.  The update box
 * is specified by the center coordinates of its first cell.  The number of
 * candidate colormap entries is returned, and their colormap indexes are
 * placed in colorlist[].
 * This routine uses Heckbert's "locally sorted search" criterion to select
 * the colors that need further consideration.
 */
{
  int numcolors = cquantize->actual_number_of_colors;
  int maxc0, maxc1, maxc2;
  int centerc0, centerc1, centerc2;
  int i, x, ncolors;
  int32_t minmaxdist, min_dist, max_dist, tdist;
  int32_t mindist[MAXNUMCOLORS];  /* min distance to colormap entry i */

  /* Compute true coordinates of update box's upper corner and center.
   * Actually we compute the coordinates of the center of the upper-corner
   * histogram cell, which are the upper bounds of the volume we care about.
   * Note that since ">>" rounds down, the "center" values may be closer to
   * min than to max; hence comparisons to them must be "<=", not "<".
   */
  maxc0 = minc0 + ((1 << BOX_C0_SHIFT) - (1 << C0_SHIFT));
  centerc0 = (minc0 + maxc0) >> 1;
  maxc1 = minc1 + ((1 << BOX_C1_SHIFT) - (1 << C1_SHIFT));
  centerc1 = (minc1 + maxc1) >> 1;
  maxc2 = minc2 + ((1 << BOX_C2_SHIFT) - (1 << C2_SHIFT));
  centerc2 = (minc2 + maxc2) >> 1;

  /* For each color in colormap, find:
   *  1. its minimum squared-distance to any point in the update box
   *     (zero if color is within update box);
   *  2. its maximum squared-distance to any point in the update box.
   * Both of these can be found by considering only the corners of the box.
   * We save the minimum distance for each color in mindist[];
   * only the smallest maximum distance is of interest.
   */
  minmaxdist = 0x7FFFFFFFL;

  for (i = 0; i < numcolors; i++) {
    /* We compute the squared-c0-distance term, then add in the other two. */
    x = cquantize->colormap[0][i];
    if (x < minc0) {
      tdist = (x - minc0) * C0_SCALE;
      min_dist = tdist*tdist;
      tdist = (x - maxc0) * C0_SCALE;
      max_dist = tdist*tdist;
    } else if (x > maxc0) {
      tdist = (x - maxc0) * C0_SCALE;
      min_dist = tdist*tdist;
      tdist = (x - minc0) * C0_SCALE;
      max_dist = tdist*tdist;
    } else {
      /* within cell range so no contribution to min_dist */
      min_dist = 0;
      if (x <= centerc0) {
        tdist = (x - maxc0) * C0_SCALE;
        max_dist = tdist*tdist;
      } else {
        tdist = (x - minc0) * C0_SCALE;
        max_dist = tdist*tdist;
      }
    }

    x = cquantize->colormap[1][i];
    if (x < minc1) {
      tdist = (x - minc1) * C1_SCALE;
      min_dist += tdist*tdist;
      tdist = (x - maxc1) * C1_SCALE;
      max_dist += tdist*tdist;
    } else if (x > maxc1) {
      tdist = (x - maxc1) * C1_SCALE;
      min_dist += tdist*tdist;
      tdist = (x - minc1) * C1_SCALE;
      max_dist += tdist*tdist;
    } else {
      /* within cell range so no contribution to min_dist */
      if (x <= centerc1) {
        tdist = (x - maxc1) * C1_SCALE;
        max_dist += tdist*tdist;
      } else {
        tdist = (x - minc1) * C1_SCALE;
        max_dist += tdist*tdist;
      }
    }

    x = cquantize->colormap[2][i];
    if (x < minc2) {
      tdist = (x - minc2) * C2_SCALE;
      min_dist += tdist*tdist;
      tdist = (x - maxc2) * C2_SCALE;
      max_dist += tdist*tdist;
    } else if (x > maxc2) {
      tdist = (x - maxc2) * C2_SCALE;
      min_dist += tdist*tdist;
      tdist = (x - minc2) * C2_SCALE;
      max_dist += tdist*tdist;
    } else {
      /* within cell range so no contribution to min_dist */
      if (x <= centerc2) {
        tdist = (x - maxc2) * C2_SCALE;
        max_dist += tdist*tdist;
      } else {
        tdist = (x - minc2) * C2_SCALE;
        max_dist += tdist*tdist;
      }
    }

    mindist[i] = min_dist;      /* save away the results */
    if (max_dist < minmaxdist)
      minmaxdist = max_dist;
  }

  /* Now we know that no cell in the update box is more than minmaxdist
   * away from some colormap entry.  Therefore, only colors that are
   * within minmaxdist of some part of the box need be considered.
   */
  ncolors = 0;
  for (i = 0; i < numcolors; i++) {
    if (mindist[i] <= minmaxdist)
      colorlist[ncolors++] = (unsigned char) i;
  }
  return ncolors;
}


static void
find_best_colors (struct quant *cquantize, int minc0, int minc1, int minc2,
                  int numcolors, unsigned char colorlist[],
                  unsigned char bestcolor[])
/* Find the closest colormap entry for each cell in the update box,
 * given the list of candidate colors prepared by find_nearby_colors.
 * Return the indexes of the closest entries in the bestcolor[] array.
 * This routine uses Thomas' incremental distance calculation method to
 * find the distance from a colormap entry to successive cells in the box.
 */
{
  int ic0, ic1, ic2;
  int i, icolor;
  register int32_t * bptr;        /* pointer into bestdist[] array */
  unsigned char * cptr;               /* pointer into bestcolor[] array */
  int32_t dist0, dist1;           /* initial distance values */
  register int32_t dist2;         /* current distance in inner loop */
  int32_t xx0, xx1;               /* distance increments */
  register int32_t xx2;
  int32_t inc0, inc1, inc2;       /* initial values for increments */
  /* This array holds the distance to the nearest-so-far color for each cell */
  int32_t bestdist[BOX_C0_ELEMS * BOX_C1_ELEMS * BOX_C2_ELEMS];

  /* Initialize best-distance for each cell of the update box */
  bptr = bestdist;
  for (i = BOX_C0_ELEMS*BOX_C1_ELEMS*BOX_C2_ELEMS-1; i >= 0; i--)
    *bptr++ = 0x7FFFFFFFL;

  /* For each color selected by find_nearby_colors,
   * compute its distance to the center of each cell in the box.
   * If that's less than best-so-far, update best distance and color number.
   */

  /* Nominal steps between cell centers ("x" in Thomas article) */
#define STEP_C0  ((1 << C0_SHIFT) * C0_SCALE)
#define STEP_C1  ((1 << C1_SHIFT) * C1_SCALE)
#define STEP_C2  ((1 << C2_SHIFT) * C2_SCALE)

  for (i = 0; i < numcolors; i++) {
    icolor = colorlist[i];
    /* Compute (square of) distance from minc0/c1/c2 to this color */
    inc0 = (minc0 - cquantize->colormap[0][icolor]) * C0_SCALE;
    dist0 = inc0*inc0;
    inc1 = (minc1 - cquantize->colormap[1][icolor]) * C1_SCALE;
    dist0 += inc1*inc1;
    inc2 = (minc2 - cquantize->colormap[2][icolor]) * C2_SCALE;
    dist0 += inc2*inc2;
    /* Form the initial difference increments */
    inc0 = inc0 * (2 * STEP_C0) + STEP_C0 * STEP_C0;
    inc1 = inc1 * (2 * STEP_C1) + STEP_C1 * STEP_C1;
    inc2 = inc2 * (2 * STEP_C2) + STEP_C2 * STEP_C2;
    /* Now loop over all cells in box, updating distance per Thomas method */
    bptr = bestdist;
    cptr = bestcolor;
    xx0 = inc0;
    for (ic0 = BOX_C0_ELEMS-1; ic0 >= 0; ic0--) {
      dist1 = dist0;
      xx1 = inc1;
      for (ic1 = BOX_C1_ELEMS-1; ic1 >= 0; ic1--) {
        dist2 = dist1;
        xx2 = inc2;
        for (ic2 = BOX_C2_ELEMS-1; ic2 >= 0; ic2--) {
          if (dist2 < *bptr) {
            *bptr = dist2;
            *cptr = icolor;
          }
          dist2 += xx2;
          xx2 += 2 * STEP_C2 * STEP_C2;
          bptr++;
          cptr++;
        }
        dist1 += xx1;
        xx1 += 2 * STEP_C1 * STEP_C1;
      }
      dist0 += xx0;
      xx0 += 2 * STEP_C0 * STEP_C0;
    }
  }
}


static void
fill_inverse_cmap (struct quant *cquantize, int c0, int c1, int c2)
/* Fill the inverse-colormap entries in the update box that contains */
/* histogram cell c0/c1/c2.  (Only that one cell MUST be filled, but */
/* we can fill as many others as we wish.) */
{
  hist3d histogram = cquantize->histogram;
  int minc0, minc1, minc2;      /* lower left corner of update box */
  int ic0, ic1, ic2;
  register unsigned char * cptr;      /* pointer into bestcolor[] array */
  register histptr cachep;      /* pointer into main cache array */
  /* This array lists the candidate colormap indexes. */
  unsigned char colorlist[MAXNUMCOLORS];
  int numcolors;                /* number of candidate colors */
  /* This array holds the actually closest colormap index for each cell. */
  unsigned char bestcolor[BOX_C0_ELEMS * BOX_C1_ELEMS * BOX_C2_ELEMS];

  /* Convert cell coordinates to update box ID */
  c0 >>= BOX_C0_LOG;
  c1 >>= BOX_C1_LOG;
  c2 >>= BOX_C2_LOG;

  /* Compute true coordinates of update box's origin corner.
   * Actually we compute the coordinates of the center of the corner
   * histogram cell, which are the lower bounds of the volume we care about.
   */
  minc0 = (c0 << BOX_C0_SHIFT) + ((1 << C0_SHIFT) >> 1);
  minc1 = (c1 << BOX_C1_SHIFT) + ((1 << C1_SHIFT) >> 1);
  minc2 = (c2 << BOX_C2_SHIFT) + ((1 << C2_SHIFT) >> 1);

  /* Determine which colormap entries are close enough to be candidates
   * for the nearest entry to some cell in the update box.
   */
  numcolors = find_nearby_colors(cquantize, minc0, minc1, minc2, colorlist);

  /* Determine the actually nearest colors. */
  find_best_colors(cquantize, minc0, minc1, minc2, numcolors, colorlist,
                   bestcolor);

  /* Save the best color numbers (plus 1) in the main cache array */
  c0 <<= BOX_C0_LOG;            /* convert ID back to base cell indexes */
  c1 <<= BOX_C1_LOG;
  c2 <<= BOX_C2_LOG;
  cptr = bestcolor;
  for (ic0 = 0; ic0 < BOX_C0_ELEMS; ic0++) {
    for (ic1 = 0; ic1 < BOX_C1_ELEMS; ic1++) {
      cachep = & histogram[c0+ic0][c1+ic1][c2];
      for (ic2 = 0; ic2 < BOX_C2_ELEMS; ic2++) {
        *cachep++ = (histcell) (*cptr++ + 1);
      }
    }
  }
}


/*
 * Map some rows of pixels to the output colormapped representation.
 */

void
pass2_no_dither (struct quant *cquantize, unsigned char **input_buf,
                 unsigned char **output_buf, int num_rows)
/* This version performs no dithering */
{
  hist3d histogram = cquantize->histogram;
  register unsigned char *inptr, *outptr;
  register histptr cachep;
  register int c0, c1, c2;
  int row;
  unsigned int col;
  unsigned int width = cquantize->output_width;

  for (row = 0; row < num_rows; row++) {
    inptr = input_buf[row];
    outptr = output_buf[row];
    for (col = width; col > 0; col--) {
      /* get pixel value and index into the cache */
      c0 = *inptr++ >> C0_SHIFT;
      c1 = *inptr++ >> C1_SHIFT;
      c2 = *inptr++ >> C2_SHIFT;
      cachep = & histogram[c0][c1][c2];
      /* If we have not seen this color before, find nearest colormap entry */
      /* and update the cache */
      if (*cachep == 0)
        fill_inverse_cmap(cquantize, c0,c1,c2);
      /* Now emit the colormap index for this cell */
      *outptr++ = (unsigned char) (*cachep - 1);
    }
  }
}

void
quant_map (struct quant *cquantize, unsigned char *inptr, unsigned char *outptr)
/* This version performs Floyd-Steinberg dithering */
{
  hist3d histogram = cquantize->histogram;
  register LOCFSERROR cur0, cur1, cur2; /* current error or pixel value */
  LOCFSERROR belowerr0, belowerr1, belowerr2; /* error for pixel below cur */
  LOCFSERROR bpreverr0, bpreverr1, bpreverr2; /* error for below/prev col */
  register FSERRPTR errorptr;   /* => fserrors[] at column before current */
  histptr cachep;
  int dir;                      /* +1 or -1 depending on direction */
  int dir3;                     /* 3*dir, for advancing inptr & errorptr */
  unsigned int col;
  unsigned int width = cquantize->output_width;
  int *error_limit = cquantize->error_limiter;
  unsigned char *colormap0 = cquantize->colormap[0];
  unsigned char *colormap1 = cquantize->colormap[1];
  unsigned char *colormap2 = cquantize->colormap[2];

  if (cquantize->on_odd_row) {
    /* work right to left in this row */
    inptr += (width-1) * 3;   /* so point to rightmost pixel */
    outptr += width-1;
    dir = -1;
    dir3 = -3;
    errorptr = cquantize->fserrors + (width+1)*3; /* => entry after last column */
    cquantize->on_odd_row = 0; /* flip for next time */
  } else {
    /* work left to right in this row */
    dir = 1;
    dir3 = 3;
    errorptr = cquantize->fserrors; /* => entry before first real column */
    cquantize->on_odd_row = 1; /* flip for next time */
  }
  /* Preset error values: no error propagated to first pixel from left */
  cur0 = cur1 = cur2 = 0;
  /* and no error propagated to row below yet */
  belowerr0 = belowerr1 = belowerr2 = 0;
  bpreverr0 = bpreverr1 = bpreverr2 = 0;

  for (col = width; col > 0; col--) {
    /* curN holds the error propagated from the previous pixel on the
     * current line.  Add the error propagated from the previous line
     * to form the complete error correction term for this pixel, and
     * round the error term (which is expressed * 16) to an integer.
     * RIGHT_SHIFT rounds towards minus infinity, so adding 8 is correct
     * for either sign of the error value.
     * Note: errorptr points to *previous* column's array entry.
     */
    cur0 = RIGHT_SHIFT(cur0 + errorptr[dir3+0] + 8, 4);
    cur1 = RIGHT_SHIFT(cur1 + errorptr[dir3+1] + 8, 4);
    cur2 = RIGHT_SHIFT(cur2 + errorptr[dir3+2] + 8, 4);
    /* Limit the error using transfer function set by init_error_limit.
     * See comments with init_error_limit for rationale.
     */
    cur0 = error_limit[cur0];
    cur1 = error_limit[cur1];
    cur2 = error_limit[cur2];
    /* Form pixel value + error, and range-limit to 0..MAXJSAMPLE.
     * The maximum error is +- MAXJSAMPLE (or less with error limiting);
     * this sets the required size of the range_limit array.
     */
    cur0 += inptr[0];
    cur1 += inptr[1];
    cur2 += inptr[2];

    if (cur0 > 255) {
    	cur0 = 255;
    } else if (cur0 < 0) {
    	cur0 = 0;
    }
    if (cur1 > 255) {
    	cur1 = 255;
    } else if (cur1 < 0) {
    	cur1 = 0;
    }
    if (cur2 > 255) {
    	cur2 = 255;
    } else if (cur2 < 0) {
    	cur2 = 0;
    }

    /* Index into the cache with adjusted pixel value */
    cachep = & histogram[cur0>>C0_SHIFT][cur1>>C1_SHIFT][cur2>>C2_SHIFT];
    /* If we have not seen this color before, find nearest colormap */
    /* entry and update the cache */
    if (*cachep == 0)
      fill_inverse_cmap(cquantize, cur0>>C0_SHIFT,cur1>>C1_SHIFT,cur2>>C2_SHIFT);
    /* Now emit the colormap index for this cell */
    { register int pixcode = *cachep - 1;
      *outptr = (unsigned char) pixcode;
      /* Compute representation error for this pixel */
      cur0 -= colormap0[pixcode];
      cur1 -= colormap1[pixcode];
      cur2 -= colormap2[pixcode];
    }
    /* Compute error fractions to be propagated to adjacent pixels.
     * Add these into the running sums, and simultaneously shift the
     * next-line error sums left by 1 column.
     */
    { register LOCFSERROR bnexterr;

      bnexterr = cur0;        /* Process component 0 */
      errorptr[0] = (FSERROR) (bpreverr0 + cur0 * 3);
      bpreverr0 = belowerr0 + cur0 * 5;
      belowerr0 = bnexterr;
      cur0 *= 7;
      bnexterr = cur1;        /* Process component 1 */
      errorptr[1] = (FSERROR) (bpreverr1 + cur1 * 3);
      bpreverr1 = belowerr1 + cur1 * 5;
      belowerr1 = bnexterr;
      cur1 *= 7;
      bnexterr = cur2;        /* Process component 2 */
      errorptr[2] = (FSERROR) (bpreverr2 + cur2 * 3);
      bpreverr2 = belowerr2 + cur2 * 5;
      belowerr2 = bnexterr;
      cur2 *= 7;
    }
    /* At this point curN contains the 7/16 error value to be propagated
     * to the next pixel on the current line, and all the errors for the
     * next line have been shifted over.  We are therefore ready to move on.
     */
    inptr += dir3;            /* Advance pixel pointers to next column */
    outptr += dir;
    errorptr += dir3;         /* advance errorptr to current column */
  }
  /* Post-loop cleanup: we must unload the final error values into the
   * final fserrors[] entry.  Note we need not unload belowerrN because
   * it is for the dummy column before or after the actual array.
   */
  errorptr[0] = (FSERROR) bpreverr0; /* unload prev errs into array */
  errorptr[1] = (FSERROR) bpreverr1;
  errorptr[2] = (FSERROR) bpreverr2;
}


/*
 * Initialize the error-limiting transfer function (lookup table).
 * The raw F-S error computation can potentially compute error values of up to
 * +- MAXJSAMPLE.  But we want the maximum correction applied to a pixel to be
 * much less, otherwise obviously wrong pixels will be created.  (Typical
 * effects include weird fringes at color-area boundaries, isolated bright
 * pixels in a dark area, etc.)  The standard advice for avoiding this problem
 * is to ensure that the "corners" of the color cube are allocated as output
 * colors; then repeated errors in the same direction cannot cause cascading
 * error buildup.  However, that only prevents the error from getting
 * completely out of hand; Aaron Giles reports that error limiting improves
 * the results even with corner colors allocated.
 * A simple clamping of the error values to about +- MAXJSAMPLE/8 works pretty
 * well, but the smoother transfer function used below is even better.  Thanks
 * to Aaron Giles for this idea.
 */

static void
init_error_limit (struct quant *cquantize)
/* Allocate and fill in the error_limiter table */
{
  int * table;
  int in, out;

  table = (int *) malloc((MAXJSAMPLE*2+1) * sizeof(int));
  table += MAXJSAMPLE;          /* so can index -MAXJSAMPLE .. +MAXJSAMPLE */
  cquantize->error_limiter = table;

#define STEPSIZE ((MAXJSAMPLE+1)/16)
  /* Map errors 1:1 up to +- MAXJSAMPLE/16 */
  out = 0;
  for (in = 0; in < STEPSIZE; in++, out++) {
    table[in] = out; table[-in] = -out;
  }
  /* Map errors 1:2 up to +- 3*MAXJSAMPLE/16 */
  for (; in < STEPSIZE*3; in++, out += (in&1) ? 0 : 1) {
    table[in] = out; table[-in] = -out;
  }
  /* Clamp the rest to final out value (which is (MAXJSAMPLE+1)/8) */
  for (; in <= MAXJSAMPLE; in++) {
    table[in] = out; table[-in] = -out;
  }
#undef STEPSIZE
}

static void
zero_histogram (struct quant *cquantize)
{
  int i;
  hist3d histogram = cquantize->histogram;

  /* Zero the histogram or inverse color map, if necessary */
  for (i = 0; i < HIST_C0_ELEMS; i++) {
    memset(histogram[i], 0, HIST_C1_ELEMS*HIST_C2_ELEMS * sizeof(histcell));
  }
}

/*
 * Finish up at the end of each pass.
 */

void
quant_gen_palette (struct quant *cquantize)
{
  /* Select the representative colors and fill in cinfo->colormap */
  cquantize->colormap = cquantize->sv_colormap;
  select_colors(cquantize, cquantize->desired);
  zero_histogram(cquantize);
}


/*
 * Module initialization routine for 2-pass color quantization.
 */

void
quant_init (struct quant *cquantize)
{
  int i, desired;
  size_t arraysize;

  cquantize->fserrors = NULL;   /* flag optional arrays not allocated */
  cquantize->error_limiter = NULL;

  /* Allocate the histogram/inverse colormap storage */
  cquantize->histogram = (hist3d)malloc(HIST_C0_ELEMS * sizeof(hist2d));
  for (i = 0; i < HIST_C0_ELEMS; i++) {
    cquantize->histogram[i] = (hist2d)malloc(HIST_C1_ELEMS*HIST_C2_ELEMS * sizeof(histcell));
  }

  zero_histogram(cquantize);

  /* Allocate storage for the completed colormap, if required.
   * We do this now since it may affect the memory manager's space
   * calculations.
   */
  /* Make sure color count is acceptable */
  desired = cquantize->desired_number_of_colors;

  cquantize->sv_colormap = malloc(3 * sizeof(unsigned char*));
  for (i = 0; i < 3; i++) {
    cquantize->sv_colormap[i] = malloc(desired);
  }
  cquantize->desired = desired;

  /* Allocate Floyd-Steinberg workspace if necessary.
   * This isn't really needed until pass 2, but again it may affect the memory
   * manager's space calculations.  Although we will cope with a later change
   * in dither_mode, we do not promise to honor max_memory_to_use if
   * dither_mode changes.
   */
  cquantize->fserrors = (FSERRPTR)malloc((cquantize->output_width + 2) * (3 * sizeof(FSERROR)));
  arraysize = (size_t) ((cquantize->output_width + 2) * (3 * sizeof(FSERROR)));
  memset(cquantize->fserrors, 0, arraysize);
  cquantize->on_odd_row = 0;

  /* Might as well create the error-limiting table too. */
  init_error_limit(cquantize);
}

void
quant_free(struct quant *cquantize)
{
  int i;

  free(cquantize->error_limiter - MAXJSAMPLE);
  free(cquantize->fserrors);

  for (i=0; i<3; i++) {
    free(cquantize->sv_colormap[i]);
  }
  free(cquantize->sv_colormap);

  for (i=0; i<HIST_C0_ELEMS; i++) {
    free(cquantize->histogram[i]);
  }
  free(cquantize->histogram);
}
