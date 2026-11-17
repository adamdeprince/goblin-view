/*
 * Copyright (c) 2026 Adam DePrince <adam.deprince@gmail.com>
 *
 * What vector.c takes from the rest of Silkie, as Odin Seat has it. This
 * file is original to Odin Seat; nothing in it is from kitty.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Silkie's configured limits on nesting, dashes and curve segments. Odin
 * Seat has no such options: 0 is no limit. */
static inline unsigned long kitty_vector_depth_limit(void) { return 0; }
static inline unsigned long kitty_vector_dash_limit(void) { return 0; }
static inline unsigned long kitty_vector_curve_limit(void) { return 0; }

/* Stereo rendering shifts an element by its depth. Odin Seat never asks for
 * a stereo render, so no element is ever shifted. */
static inline double
spatial_requested_disparity(double depth, double depth_pixels, double multiplier,
    double positive, double negative, double fov)
{
	(void)depth; (void)depth_pixels; (void)multiplier;
	(void)positive; (void)negative; (void)fov;
	return 0.;
}

/* Every entry of a verstable table, in turn. */
#define vt_create_for_loop(itr_type, itr, table) \
	for (itr_type itr = vt_first(table); !vt_is_end(itr); itr = vt_next(itr))
