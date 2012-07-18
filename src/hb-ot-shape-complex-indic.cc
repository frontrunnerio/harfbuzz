/*
 * Copyright © 2011,2012  Google, Inc.
 *
 *  This is part of HarfBuzz, a text shaping library.
 *
 * Permission is hereby granted, without written agreement and without
 * license or royalty fees, to use, copy, modify, and distribute this
 * software and its documentation for any purpose, provided that the
 * above copyright notice and the following two paragraphs appear in
 * all copies of this software.
 *
 * IN NO EVENT SHALL THE COPYRIGHT HOLDER BE LIABLE TO ANY PARTY FOR
 * DIRECT, INDIRECT, SPECIAL, INCIDENTAL, OR CONSEQUENTIAL DAMAGES
 * ARISING OUT OF THE USE OF THIS SOFTWARE AND ITS DOCUMENTATION, EVEN
 * IF THE COPYRIGHT HOLDER HAS BEEN ADVISED OF THE POSSIBILITY OF SUCH
 * DAMAGE.
 *
 * THE COPYRIGHT HOLDER SPECIFICALLY DISCLAIMS ANY WARRANTIES, INCLUDING,
 * BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND
 * FITNESS FOR A PARTICULAR PURPOSE.  THE SOFTWARE PROVIDED HEREUNDER IS
 * ON AN "AS IS" BASIS, AND THE COPYRIGHT HOLDER HAS NO OBLIGATION TO
 * PROVIDE MAINTENANCE, SUPPORT, UPDATES, ENHANCEMENTS, OR MODIFICATIONS.
 *
 * Google Author(s): Behdad Esfahbod
 */

#include "hb-ot-shape-complex-indic-private.hh"
#include "hb-ot-shape-private.hh"

#define OLD_INDIC_TAG(script) (((hb_tag_t) script) | 0x20000000)
#define IS_OLD_INDIC_TAG(tag) ( \
				(tag) == OLD_INDIC_TAG (HB_SCRIPT_BENGALI) || \
				(tag) == OLD_INDIC_TAG (HB_SCRIPT_DEVANAGARI) || \
				(tag) == OLD_INDIC_TAG (HB_SCRIPT_GUJARATI) || \
				(tag) == OLD_INDIC_TAG (HB_SCRIPT_GURMUKHI) || \
				(tag) == OLD_INDIC_TAG (HB_SCRIPT_KANNADA) || \
				(tag) == OLD_INDIC_TAG (HB_SCRIPT_MALAYALAM) || \
				(tag) == OLD_INDIC_TAG (HB_SCRIPT_ORIYA) || \
				(tag) == OLD_INDIC_TAG (HB_SCRIPT_TAMIL) || \
				(tag) == OLD_INDIC_TAG (HB_SCRIPT_TELUGU) \
			      )
struct indic_options_t
{
  int initialized : 1;
  int uniscribe_bug_compatible : 1;
};

union indic_options_union_t {
  int i;
  indic_options_t opts;
};
ASSERT_STATIC (sizeof (int) == sizeof (indic_options_union_t));

static indic_options_union_t
indic_options_init (void)
{
  indic_options_union_t u;
  u.i = 0;
  u.opts.initialized = 1;

  char *c = getenv ("HB_OT_INDIC_OPTIONS");
  u.opts.uniscribe_bug_compatible = c && strstr (c, "uniscribe-bug-compatible");

  return u;
}

inline indic_options_t
indic_options (void)
{
  static indic_options_union_t options;

  if (unlikely (!options.i)) {
    /* This is idempotent and threadsafe. */
    options = indic_options_init ();
  }

  return options.opts;
}


static int
compare_codepoint (const void *pa, const void *pb)
{
  hb_codepoint_t a = * (hb_codepoint_t *) pa;
  hb_codepoint_t b = * (hb_codepoint_t *) pb;

  return a < b ? -1 : a == b ? 0 : +1;
}

static indic_position_t
consonant_position (hb_codepoint_t u)
{
  consonant_position_t *record;

  /* Khmer does not have pre-base half forms. */
  if (0x1780 <= u && u <= 0x17FF)
    return POS_BELOW_C;

  record = (consonant_position_t *) bsearch (&u, consonant_positions,
					     ARRAY_LENGTH (consonant_positions),
					     sizeof (consonant_positions[0]),
					     compare_codepoint);

  return record ? record->position : POS_BASE_C;
}

static bool
is_ra (hb_codepoint_t u)
{
  return !!bsearch (&u, ra_chars,
		    ARRAY_LENGTH (ra_chars),
		    sizeof (ra_chars[0]),
		    compare_codepoint);
}

static bool
is_joiner (const hb_glyph_info_t &info)
{
  return !!(FLAG (info.indic_category()) & (FLAG (OT_ZWJ) | FLAG (OT_ZWNJ)));
}

static bool
is_consonant (const hb_glyph_info_t &info)
{
  /* Note:
   *
   * We treat Vowels and placeholders as if they were consonants.  This is safe because Vowels
   * cannot happen in a consonant syllable.  The plus side however is, we can call the
   * consonant syllable logic from the vowel syllable function and get it all right! */
  return !!(FLAG (info.indic_category()) & (FLAG (OT_C) | FLAG (OT_Ra) | FLAG (OT_V) | FLAG (OT_NBSP) | FLAG (OT_DOTTEDCIRCLE)));
}

static bool
is_halant_or_coeng (const hb_glyph_info_t &info)
{
  return !!(FLAG (info.indic_category()) & (FLAG (OT_H) | FLAG (OT_Coeng)));
}

struct feature_list_t {
  hb_tag_t tag;
  hb_bool_t is_global;
};

/* These features are applied one at a time, given the order in this table. */
static const feature_list_t
indic_basic_features[] =
{
  {HB_TAG('n','u','k','t'), true},
  {HB_TAG('a','k','h','n'), false},
  {HB_TAG('r','p','h','f'), false},
  {HB_TAG('r','k','r','f'), true},
  {HB_TAG('p','r','e','f'), false},
  {HB_TAG('b','l','w','f'), false},
  {HB_TAG('h','a','l','f'), false},
  {HB_TAG('a','b','v','f'), false},
  {HB_TAG('p','s','t','f'), false},
  {HB_TAG('c','f','a','r'), false},
  {HB_TAG('c','j','c','t'), false},
  {HB_TAG('v','a','t','u'), true},
};

/* Same order as the indic_basic_features array */
enum {
  _NUKT,
  AKHN,
  RPHF,
  _RKRF,
  PREF,
  BLWF,
  HALF,
  ABVF,
  PSTF,
  CFAR,
  CJCT,
  VATU
};

/* These features are applied all at once. */
static const feature_list_t
indic_other_features[] =
{
  {HB_TAG('i','n','i','t'), false},
  {HB_TAG('p','r','e','s'), true},
  {HB_TAG('a','b','v','s'), true},
  {HB_TAG('b','l','w','s'), true},
  {HB_TAG('p','s','t','s'), true},
  {HB_TAG('h','a','l','n'), true},

  {HB_TAG('d','i','s','t'), true},
  {HB_TAG('a','b','v','m'), true},
  {HB_TAG('b','l','w','m'), true},
};


static void
initial_reordering (const hb_ot_map_t *map,
		    hb_face_t *face,
		    hb_buffer_t *buffer,
		    void *user_data HB_UNUSED);
static void
final_reordering (const hb_ot_map_t *map,
		  hb_face_t *face,
		  hb_buffer_t *buffer,
		  void *user_data HB_UNUSED);

void
_hb_ot_shape_complex_collect_features_indic (hb_ot_map_builder_t *map,
					     const hb_segment_properties_t *props HB_UNUSED)
{
  map->add_bool_feature (HB_TAG('l','o','c','l'));
  /* The Indic specs do not require ccmp, but we apply it here since if
   * there is a use of it, it's typically at the beginning. */
  map->add_bool_feature (HB_TAG('c','c','m','p'));

  map->add_gsub_pause (initial_reordering, NULL);

  for (unsigned int i = 0; i < ARRAY_LENGTH (indic_basic_features); i++) {
    map->add_bool_feature (indic_basic_features[i].tag, indic_basic_features[i].is_global);
    map->add_gsub_pause (NULL, NULL);
  }

  map->add_gsub_pause (final_reordering, NULL);

  for (unsigned int i = 0; i < ARRAY_LENGTH (indic_other_features); i++)
    map->add_bool_feature (indic_other_features[i].tag, indic_other_features[i].is_global);
}

void
_hb_ot_shape_complex_override_features_indic (hb_ot_map_builder_t *map,
					      const hb_segment_properties_t *props HB_UNUSED)
{
  /* Uniscribe does not apply 'kern'. */
  if (indic_options ().uniscribe_bug_compatible)
    map->add_feature (HB_TAG('k','e','r','n'), 0, true);
}


hb_ot_shape_normalization_mode_t
_hb_ot_shape_complex_normalization_preference_indic (void)
{
  /* We want split matras decomposed by the common shaping logic. */
  return HB_OT_SHAPE_NORMALIZATION_MODE_DECOMPOSED;
}


void
_hb_ot_shape_complex_setup_masks_indic (hb_ot_map_t *map HB_UNUSED,
					hb_buffer_t *buffer,
					hb_font_t *font HB_UNUSED)
{
  HB_BUFFER_ALLOCATE_VAR (buffer, indic_category);
  HB_BUFFER_ALLOCATE_VAR (buffer, indic_position);

  /* We cannot setup masks here.  We save information about characters
   * and setup masks later on in a pause-callback. */

  unsigned int count = buffer->len;
  for (unsigned int i = 0; i < count; i++)
  {
    hb_glyph_info_t &info = buffer->info[i];
    unsigned int type = get_indic_categories (info.codepoint);

    info.indic_category() = type & 0x0F;
    info.indic_position() = type >> 4;

    /* The spec says U+0952 is OT_A.  However, testing shows that Uniscribe
     * treats U+0951..U+0952 all as OT_VD.
     * TESTS:
     * U+092E,U+0947,U+0952
     * U+092E,U+0952,U+0947
     * U+092E,U+0947,U+0951
     * U+092E,U+0951,U+0947
     * */
    if (unlikely (hb_in_range<hb_codepoint_t> (info.codepoint, 0x0951, 0x0954)))
      info.indic_category() = OT_VD;

    if (info.indic_category() == OT_X &&
	unlikely (hb_in_range<hb_codepoint_t> (info.codepoint, 0x17CB, 0x17D2))) /* Khmer Various signs */
      info.indic_category() = OT_N;

    /* Khmer Virama is different since it can be used to form a final consonant. */
    if (unlikely (info.codepoint == 0x17D2))
      info.indic_category() = OT_Coeng;

    if (info.indic_category() == OT_Repha) {
      /* There are two kinds of characters marked as Repha:
       * - The ones that are GenCat=Mn are already positioned visually, ie. after base. (eg. Khmer)
       * - The ones that are GenCat=Lo is encoded logically, ie. beginning of syllable. (eg. Malayalam)
       *
       * We recategorize the first kind to look like a Nukta and attached to the base directly.
       */
      if (_hb_glyph_info_get_general_category (&info) == HB_UNICODE_GENERAL_CATEGORY_NON_SPACING_MARK)
        info.indic_category() = OT_N;
    }


    /* Assign positions... */
    if (is_consonant (info)) {
      info.indic_position() = consonant_position (info.codepoint);
      if (is_ra (info.codepoint))
	info.indic_category() = OT_Ra;
    } else if (info.indic_category() == OT_RS) {
      info.indic_position() = POS_ABOVE_M;
    } else if (info.indic_category() == OT_SM ||
	       info.indic_category() == OT_VD) {
      info.indic_position() = POS_SMVD;
    } else if (unlikely (info.codepoint == 0x200C))
      info.indic_category() = OT_ZWNJ;
    else if (unlikely (info.codepoint == 0x200D))
      info.indic_category() = OT_ZWJ;
    else if (unlikely (info.codepoint == 0x25CC))
      info.indic_category() = OT_DOTTEDCIRCLE;
  }
}

static int
compare_indic_order (const hb_glyph_info_t *pa, const hb_glyph_info_t *pb)
{
  int a = pa->indic_position();
  int b = pb->indic_position();

  return a < b ? -1 : a == b ? 0 : +1;
}

/* Rules from:
 * https://www.microsoft.com/typography/otfntdev/devanot/shaping.aspx */

static void
initial_reordering_consonant_syllable (const hb_ot_map_t *map, hb_buffer_t *buffer, hb_mask_t *basic_mask_array,
				       unsigned int start, unsigned int end)
{
  hb_glyph_info_t *info = buffer->info;


  /* 1. Find base consonant:
   *
   * The shaping engine finds the base consonant of the syllable, using the
   * following algorithm: starting from the end of the syllable, move backwards
   * until a consonant is found that does not have a below-base or post-base
   * form (post-base forms have to follow below-base forms), or that is not a
   * pre-base reordering Ra, or arrive at the first consonant. The consonant
   * stopped at will be the base.
   *
   *   o If the syllable starts with Ra + Halant (in a script that has Reph)
   *     and has more than one consonant, Ra is excluded from candidates for
   *     base consonants.
   */

  unsigned int base = end;
  bool has_reph = false;

  {
    /* -> If the syllable starts with Ra + Halant (in a script that has Reph)
     *    and has more than one consonant, Ra is excluded from candidates for
     *    base consonants. */
    unsigned int limit = start;
    if (basic_mask_array[RPHF] &&
	start + 3 <= end &&
	info[start].indic_category() == OT_Ra &&
	info[start + 1].indic_category() == OT_H &&
	!is_joiner (info[start + 2]))
    {
      limit += 2;
      base = start;
      has_reph = true;
    };

     enum base_position_t {
       BASE_FIRST,
       BASE_LAST
     } base_pos;

    switch ((hb_tag_t) buffer->props.script)
    {
      case HB_SCRIPT_KHMER:
	base_pos = BASE_FIRST;
	break;

      default:
	base_pos = BASE_LAST;
	break;
    }

    if (base_pos == BASE_LAST)
    {
      /* -> starting from the end of the syllable, move backwards */
      unsigned int i = end;
      do {
	i--;
	/* -> until a consonant is found */
	if (is_consonant (info[i]))
	{
	  /* -> that does not have a below-base or post-base form
	   * (post-base forms have to follow below-base forms), */
	  if (info[i].indic_position() != POS_BELOW_C &&
	      info[i].indic_position() != POS_POST_C)
	  {
	    base = i;
	    break;
	  }

	  /* -> or that is not a pre-base reordering Ra,
	   *
	   * IMPLEMENTATION NOTES:
	   *
	   * Our pre-base reordering Ra's are marked POS_BELOW, so will be skipped
	   * by the logic above already.
	   */

	  /* -> or arrive at the first consonant. The consonant stopped at will
	   * be the base. */
	  base = i;
	}
	else
	  if (is_joiner (info[i]))
	    break;
      } while (i > limit);
    }
    else
    {
      /* In scripts without half forms (eg. Khmer), the first consonant is always the base. */

      if (!has_reph)
	base = limit;
    }

    if (base < start)
      base = start; /* Just in case... */


    /* -> If the syllable starts with Ra + Halant (in a script that has Reph)
     *    and has more than one consonant, Ra is excluded from candidates for
     *    base consonants. */
    if (has_reph && base == start) {
      /* Have no other consonant, so Reph is not formed and Ra becomes base. */
      has_reph = false;
    }
  }


  /* 2. Decompose and reorder Matras:
   *
   * Each matra and any syllable modifier sign in the cluster are moved to the
   * appropriate position relative to the consonant(s) in the cluster. The
   * shaping engine decomposes two- or three-part matras into their constituent
   * parts before any repositioning. Matra characters are classified by which
   * consonant in a conjunct they have affinity for and are reordered to the
   * following positions:
   *
   *   o Before first half form in the syllable
   *   o After subjoined consonants
   *   o After post-form consonant
   *   o After main consonant (for above marks)
   *
   * IMPLEMENTATION NOTES:
   *
   * The normalize() routine has already decomposed matras for us, so we don't
   * need to worry about that.
   */


  /* 3.  Reorder marks to canonical order:
   *
   * Adjacent nukta and halant or nukta and vedic sign are always repositioned
   * if necessary, so that the nukta is first.
   *
   * IMPLEMENTATION NOTES:
   *
   * We don't need to do this: the normalize() routine already did this for us.
   */


  /* Reorder characters */

  for (unsigned int i = start; i < base; i++)
    info[i].indic_position() = POS_PRE_C;

  if (base < end)
    info[base].indic_position() = POS_BASE_C;

  /* Mark final consonants.  A final consonant is one appearing after a matra,
   * like in Khmer. */
  for (unsigned int i = base + 1; i < end; i++)
    if (info[i].indic_category() == OT_M) {
      for (unsigned int j = i + 1; j < end; j++)
        if (is_consonant (info[j])) {
	  info[j].indic_position() = POS_FINAL_C;
	  break;
	}
      break;
    }

  /* Handle beginning Ra */
  if (has_reph)
    info[start].indic_position() = POS_RA_TO_BECOME_REPH;

  /* For old-style Indic script tags, move the first post-base Halant after
   * last consonant. */
  if (IS_OLD_INDIC_TAG (map->get_chosen_script (0))) {
    for (unsigned int i = base + 1; i < end; i++)
      if (info[i].indic_category() == OT_H) {
        unsigned int j;
        for (j = end - 1; j > i; j--)
	  if (is_consonant (info[j]))
	    break;
	if (j > i) {
	  /* Move Halant to after last consonant. */
	  hb_glyph_info_t t = info[i];
	  memmove (&info[i], &info[i + 1], (j - i) * sizeof (info[0]));
	  info[j] = t;
	}
        break;
      }
  }

  /* Attach ZWJ, ZWNJ, nukta, and halant to previous char to move with them. */
  if (!indic_options ().uniscribe_bug_compatible)
  {
    /* Please update the Uniscribe branch when touching this! */
    for (unsigned int i = start + 1; i < end; i++)
      if ((FLAG (info[i].indic_category()) & (FLAG (OT_ZWNJ) | FLAG (OT_ZWJ) | FLAG (OT_N) | FLAG (OT_RS) | FLAG (OT_H))))
	info[i].indic_position() = info[i - 1].indic_position();
  } else {
    /*
     * Uniscribe doesn't move the Halant with Left Matra.
     * TEST: U+092B,U+093F,U+094DE
     */
    /* Please update the non-Uniscribe branch when touching this! */
    for (unsigned int i = start + 1; i < end; i++)
      if ((FLAG (info[i].indic_category()) & (FLAG (OT_ZWNJ) | FLAG (OT_ZWJ) | FLAG (OT_N) | FLAG (OT_RS) | FLAG (OT_H)))) {
	info[i].indic_position() = info[i - 1].indic_position();
	if (info[i].indic_category() == OT_H && info[i].indic_position() == POS_PRE_M)
	  for (unsigned int j = i; j > start; j--)
	    if (info[j - 1].indic_position() != POS_PRE_M) {
	      info[i].indic_position() = info[j - 1].indic_position();
	      break;
	    }
      }
  }
  /* Re-attach ZWJ, ZWNJ, and halant to next char, for after-base consonants. */
  {
    unsigned int last_halant = end;
    for (unsigned int i = base + 1; i < end; i++)
      if (is_halant_or_coeng (info[i]))
        last_halant = i;
      else if (is_consonant (info[i])) {
	for (unsigned int j = last_halant; j < i; j++)
	  info[j].indic_position() = info[i].indic_position();
      }
  }

  /* We do bubble-sort, skip malicious clusters attempts */
  if (end - start < 64)
  {
    /* Sit tight, rock 'n roll! */
    hb_bubble_sort (info + start, end - start, compare_indic_order);
    /* Find base again */
    base = end;
    for (unsigned int i = start; i < end; i++)
      if (info[i].indic_position() == POS_BASE_C) {
        base = i;
	break;
      }
  }

  /* Setup masks now */

  {
    hb_mask_t mask;

    /* Reph */
    for (unsigned int i = start; i < end && info[i].indic_position() == POS_RA_TO_BECOME_REPH; i++)
      info[i].mask |= basic_mask_array[RPHF];

    /* Pre-base */
    mask = basic_mask_array[HALF] | basic_mask_array[AKHN] | basic_mask_array[CJCT];
    for (unsigned int i = start; i < base; i++)
      info[i].mask  |= mask;
    /* Base */
    mask = basic_mask_array[AKHN] | basic_mask_array[CJCT];
    if (base < end)
      info[base].mask |= mask;
    /* Post-base */
    mask = basic_mask_array[BLWF] | basic_mask_array[ABVF] | basic_mask_array[PSTF] | basic_mask_array[CJCT];
    for (unsigned int i = base + 1; i < end; i++)
      info[i].mask  |= mask;
  }

  /* XXX This will not match for old-Indic spec since the Halant-Ra order is reversed already. */
  if (basic_mask_array[PREF] && base + 3 <= end)
  {
    /* Find a Halant,Ra sequence and mark it fore pre-base reordering processing. */
    for (unsigned int i = base + 1; i + 1 < end; i++)
      if (is_halant_or_coeng (info[i]) &&
	  info[i + 1].indic_category() == OT_Ra)
      {
	info[i++].mask |= basic_mask_array[PREF];
	info[i++].mask |= basic_mask_array[PREF];

	/* Mark the subsequent stuff with 'cfar'.  Used in Khmer.
	 * Read the feature spec.
	 * This allows distinguishing the following cases with MS Khmer fonts:
	 * U+1784,U+17D2,U+179A,U+17D2,U+1782
	 * U+1784,U+17D2,U+1782,U+17D2,U+179A
	 */
	for (; i < end; i++)
	  info[i].mask |= basic_mask_array[CFAR];

	break;
      }
  }

  /* Apply ZWJ/ZWNJ effects */
  for (unsigned int i = start + 1; i < end; i++)
    if (is_joiner (info[i])) {
      bool non_joiner = info[i].indic_category() == OT_ZWNJ;
      unsigned int j = i;

      do {
	j--;

	info[j].mask &= ~basic_mask_array[CJCT];
	if (non_joiner)
	  info[j].mask &= ~basic_mask_array[HALF];

      } while (j > start && !is_consonant (info[j]));
    }
}


static void
initial_reordering_vowel_syllable (const hb_ot_map_t *map,
				   hb_buffer_t *buffer,
				   hb_mask_t *basic_mask_array,
				   unsigned int start, unsigned int end)
{
  /* We made the vowels look like consonants.  So let's call the consonant logic! */
  initial_reordering_consonant_syllable (map, buffer, basic_mask_array, start, end);
}

static void
initial_reordering_standalone_cluster (const hb_ot_map_t *map,
				       hb_buffer_t *buffer,
				       hb_mask_t *basic_mask_array,
				       unsigned int start, unsigned int end)
{
  /* We treat NBSP/dotted-circle as if they are consonants, so we should just chain.
   * Only if not in compatibility mode that is... */

  if (indic_options ().uniscribe_bug_compatible)
  {
    /* For dotted-circle, this is what Uniscribe does:
     * If dotted-circle is the last glyph, it just does nothing.
     * Ie. It doesn't form Reph. */
    if (buffer->info[end - 1].indic_category() == OT_DOTTEDCIRCLE)
      return;
  }

  initial_reordering_consonant_syllable (map, buffer, basic_mask_array, start, end);
}

static void
initial_reordering_non_indic (const hb_ot_map_t *map HB_UNUSED,
			      hb_buffer_t *buffer HB_UNUSED,
			      hb_mask_t *basic_mask_array HB_UNUSED,
			      unsigned int start HB_UNUSED, unsigned int end HB_UNUSED)
{
  /* Nothing to do right now.  If we ever switch to using the output
   * buffer in the reordering process, we'd need to next_glyph() here. */
}

#include "hb-ot-shape-complex-indic-machine.hh"

static void
initial_reordering (const hb_ot_map_t *map,
		    hb_face_t *face HB_UNUSED,
		    hb_buffer_t *buffer,
		    void *user_data HB_UNUSED)
{
  hb_mask_t basic_mask_array[ARRAY_LENGTH (indic_basic_features)] = {0};
  unsigned int num_masks = ARRAY_LENGTH (indic_basic_features);
  for (unsigned int i = 0; i < num_masks; i++)
    basic_mask_array[i] = map->get_1_mask (indic_basic_features[i].tag);

  find_syllables (map, buffer, basic_mask_array);
}

static void
final_reordering_syllable (hb_buffer_t *buffer,
			   hb_mask_t init_mask, hb_mask_t pref_mask,
			   unsigned int start, unsigned int end)
{
  hb_glyph_info_t *info = buffer->info;

  /* 4. Final reordering:
   *
   * After the localized forms and basic shaping forms GSUB features have been
   * applied (see below), the shaping engine performs some final glyph
   * reordering before applying all the remaining font features to the entire
   * cluster.
   */

  /* Find base again */
  unsigned int base = end;
  for (unsigned int i = start; i < end; i++)
    if (info[i].indic_position() == POS_BASE_C) {
      base = i;
      break;
    }

  unsigned int start_of_last_cluster = base;

  /*   o Reorder matras:
   *
   *     If a pre-base matra character had been reordered before applying basic
   *     features, the glyph can be moved closer to the main consonant based on
   *     whether half-forms had been formed. Actual position for the matra is
   *     defined as “after last standalone halant glyph, after initial matra
   *     position and before the main consonant”. If ZWJ or ZWNJ follow this
   *     halant, position is moved after it.
   */

  if (start < base) /* Otherwise there can't be any pre-base matra characters. */
  {
    unsigned int new_pos = base - 1;
    while (new_pos > start &&
	   !(FLAG (info[new_pos].indic_category()) & (FLAG (OT_M) | FLAG (OT_H) | FLAG (OT_Coeng))))
      new_pos--;
    /* If we found no Halant we are done.  Otherwise only proceed if the Halant does
     * not belong to the Matra itself! */
    if (is_halant_or_coeng (info[new_pos]) &&
	info[new_pos].indic_position() != POS_PRE_M) {
      /* -> If ZWJ or ZWNJ follow this halant, position is moved after it. */
      if (new_pos + 1 < end && is_joiner (info[new_pos + 1]))
	new_pos++;

      /* Now go see if there's actually any matras... */
      for (unsigned int i = new_pos; i > start; i--)
	if (info[i - 1].indic_position () == POS_PRE_M)
	{
	  unsigned int old_pos = i - 1;
	  hb_glyph_info_t tmp = info[old_pos];
	  memmove (&info[old_pos], &info[old_pos + 1], (new_pos - old_pos) * sizeof (info[0]));
	  info[new_pos] = tmp;
	  start_of_last_cluster = MIN (new_pos, start_of_last_cluster);
	  new_pos--;
	}
    }
  }


  /*   o Reorder reph:
   *
   *     Reph’s original position is always at the beginning of the syllable,
   *     (i.e. it is not reordered at the character reordering stage). However,
   *     it will be reordered according to the basic-forms shaping results.
   *     Possible positions for reph, depending on the script, are; after main,
   *     before post-base consonant forms, and after post-base consonant forms.
   */

  /* If there's anything after the Ra that has the REPH pos, it ought to be halant.
   * Which means that the font has failed to ligate the Reph.  In which case, we
   * shouldn't move. */
  if (start + 1 < end &&
      info[start].indic_position() == POS_RA_TO_BECOME_REPH &&
      info[start + 1].indic_position() != POS_RA_TO_BECOME_REPH)
  {
      unsigned int new_reph_pos;

     enum reph_position_t {
       REPH_AFTER_MAIN,
       REPH_BEFORE_SUBSCRIPT,
       REPH_AFTER_SUBSCRIPT,
       REPH_BEFORE_POSTSCRIPT,
       REPH_AFTER_POSTSCRIPT
     } reph_pos;

     /* XXX Figure out old behavior too */
     switch ((hb_tag_t) buffer->props.script)
     {
       case HB_SCRIPT_MALAYALAM:
       case HB_SCRIPT_ORIYA:
	 reph_pos = REPH_AFTER_MAIN;
	 break;

       case HB_SCRIPT_GURMUKHI:
	 reph_pos = REPH_BEFORE_SUBSCRIPT;
	 break;

       case HB_SCRIPT_BENGALI:
	 reph_pos = REPH_AFTER_SUBSCRIPT;
	 break;

       default:
       case HB_SCRIPT_DEVANAGARI:
       case HB_SCRIPT_GUJARATI:
	 reph_pos = REPH_BEFORE_POSTSCRIPT;
	 break;

       case HB_SCRIPT_KANNADA:
       case HB_SCRIPT_TAMIL:
       case HB_SCRIPT_TELUGU:
	 reph_pos = REPH_AFTER_POSTSCRIPT;
	 break;
     }

    /*       1. If reph should be positioned after post-base consonant forms,
     *          proceed to step 5.
     */
    if (reph_pos == REPH_AFTER_POSTSCRIPT)
    {
      goto reph_step_5;
    }

    /*       2. If the reph repositioning class is not after post-base: target
     *          position is after the first explicit halant glyph between the
     *          first post-reph consonant and last main consonant. If ZWJ or ZWNJ
     *          are following this halant, position is moved after it. If such
     *          position is found, this is the target position. Otherwise,
     *          proceed to the next step.
     *
     *          Note: in old-implementation fonts, where classifications were
     *          fixed in shaping engine, there was no case where reph position
     *          will be found on this step.
     */
    {
      new_reph_pos = start + 1;
      while (new_reph_pos < base && !is_halant_or_coeng (info[new_reph_pos]))
	new_reph_pos++;

      if (new_reph_pos < base && is_halant_or_coeng (info[new_reph_pos])) {
	/* ->If ZWJ or ZWNJ are following this halant, position is moved after it. */
	if (new_reph_pos + 1 < base && is_joiner (info[new_reph_pos + 1]))
	  new_reph_pos++;
	goto reph_move;
      }
    }

    /*       3. If reph should be repositioned after the main consonant: find the
     *          first consonant not ligated with main, or find the first
     *          consonant that is not a potential pre-base reordering Ra.
     */
    if (reph_pos == REPH_AFTER_MAIN)
    {
      new_reph_pos = base;
      /* XXX Skip potential pre-base reordering Ra. */
      while (new_reph_pos < end &&
	     !( FLAG (info[new_reph_pos + 1].indic_position()) & (FLAG (POS_BELOW_C) | FLAG (POS_POST_C) | FLAG (POS_POST_M) | FLAG (POS_SMVD))))
	new_reph_pos++;
      if (new_reph_pos < end)
        goto reph_move;
    }

    /*       4. If reph should be positioned before post-base consonant, find
     *          first post-base classified consonant not ligated with main. If no
     *          consonant is found, the target position should be before the
     *          first matra, syllable modifier sign or vedic sign.
     */
    /* This is our take on what step 4 is trying to say (and failing, BADLY). */
    if (reph_pos == REPH_AFTER_SUBSCRIPT)
    {
      new_reph_pos = base;
      while (new_reph_pos < end &&
	     !( FLAG (info[new_reph_pos + 1].indic_position()) & (FLAG (POS_POST_C) | FLAG (POS_POST_M) | FLAG (POS_SMVD))))
	new_reph_pos++;
      if (new_reph_pos < end)
        goto reph_move;
    }

    /*       5. If no consonant is found in steps 3 or 4, move reph to a position
     *          immediately before the first post-base matra, syllable modifier
     *          sign or vedic sign that has a reordering class after the intended
     *          reph position. For example, if the reordering position for reph
     *          is post-main, it will skip above-base matras that also have a
     *          post-main position.
     */
    reph_step_5:
    {
      /* XXX */
    }

    /*       6. Otherwise, reorder reph to the end of the syllable.
     */
    {
      new_reph_pos = end - 1;
      while (new_reph_pos > start && info[new_reph_pos].indic_position() == POS_SMVD)
	new_reph_pos--;

      /*
       * If the Reph is to be ending up after a Matra,Halant sequence,
       * position it before that Halant so it can interact with the Matra.
       * However, if it's a plain Consonant,Halant we shouldn't do that.
       * Uniscribe doesn't do this.
       * TEST: U+0930,U+094D,U+0915,U+094B,U+094D
       */
      if (!indic_options ().uniscribe_bug_compatible &&
	  unlikely (is_halant_or_coeng (info[new_reph_pos]))) {
	for (unsigned int i = base + 1; i < new_reph_pos; i++)
	  if (info[i].indic_category() == OT_M) {
	    /* Ok, got it. */
	    new_reph_pos--;
	  }
      }
      goto reph_move;
    }

    reph_move:
    {
      /* Move */
      hb_glyph_info_t reph = info[start];
      memmove (&info[start], &info[start + 1], (new_reph_pos - start) * sizeof (info[0]));
      info[new_reph_pos] = reph;
      start_of_last_cluster = start; /* Yay, one big cluster! */
    }
  }


  /*   o Reorder pre-base reordering consonants:
   *
   *     If a pre-base reordering consonant is found, reorder it according to
   *     the following rules:
   */

  if (pref_mask && base + 1 < end) /* Otherwise there can't be any pre-base reordering Ra. */
  {
    for (unsigned int i = base + 1; i < end; i++)
      if ((info[i].mask & pref_mask) != 0)
      {
	/*       1. Only reorder a glyph produced by substitution during application
	 *          of the <pref> feature. (Note that a font may shape a Ra consonant with
	 *          the feature generally but block it in certain contexts.)
	 */
	if (i + 1 == end || (info[i + 1].mask & pref_mask) == 0)
	{
	  /*
	   *       2. Try to find a target position the same way as for pre-base matra.
	   *          If it is found, reorder pre-base consonant glyph.
	   *
	   *       3. If position is not found, reorder immediately before main
	   *          consonant.
	   */

	  unsigned int new_pos = base;
	  while (new_pos > start + 1 &&
		 !(FLAG (info[new_pos - 1].indic_category()) & (FLAG (OT_M) | FLAG (OT_H) | FLAG (OT_Coeng))))
	    new_pos--;

	  if (new_pos > start && is_halant_or_coeng (info[new_pos - 1]))
	    /* -> If ZWJ or ZWNJ follow this halant, position is moved after it. */
	    if (new_pos < end && is_joiner (info[new_pos]))
	      new_pos++;

	  {
	    unsigned int old_pos = i;
	    hb_glyph_info_t tmp = info[old_pos];
	    memmove (&info[new_pos + 1], &info[new_pos], (old_pos - new_pos) * sizeof (info[0]));
	    info[new_pos] = tmp;
	    start_of_last_cluster = MIN (new_pos, start_of_last_cluster);
	  }
	}

        break;
      }
  }


  /* Apply 'init' to the Left Matra if it's a word start. */
  if (info[start].indic_position () == POS_PRE_M &&
      (!start ||
       !(FLAG (_hb_glyph_info_get_general_category (&info[start - 1])) &
	 (FLAG (HB_UNICODE_GENERAL_CATEGORY_LOWERCASE_LETTER) |
	  FLAG (HB_UNICODE_GENERAL_CATEGORY_MODIFIER_LETTER) |
	  FLAG (HB_UNICODE_GENERAL_CATEGORY_OTHER_LETTER) |
	  FLAG (HB_UNICODE_GENERAL_CATEGORY_TITLECASE_LETTER) |
	  FLAG (HB_UNICODE_GENERAL_CATEGORY_UPPERCASE_LETTER) |
	  FLAG (HB_UNICODE_GENERAL_CATEGORY_SPACING_MARK) |
	  FLAG (HB_UNICODE_GENERAL_CATEGORY_ENCLOSING_MARK) |
	  FLAG (HB_UNICODE_GENERAL_CATEGORY_NON_SPACING_MARK)))))
    info[start].mask |= init_mask;



  /* Finish off the clusters and go home! */

  if (!indic_options ().uniscribe_bug_compatible)
  {
    /* This is what Uniscribe does.  Ie. add cluster boundaries after Halant,ZWNJ.
     * This means, half forms are submerged into the main consonants cluster.
     * This is unnecessary, and makes cursor positioning harder, but that's what
     * Uniscribe does. */
    unsigned int cluster_start = start;
    for (unsigned int i = start + 1; i < start_of_last_cluster; i++)
      if (is_halant_or_coeng (info[i - 1]) && info[i].indic_category() == OT_ZWNJ) {
        i++;
	buffer->merge_clusters (cluster_start, i);
	cluster_start = i;
      }
    start_of_last_cluster = cluster_start;
  }

  buffer->merge_clusters (start_of_last_cluster, end);
}


static void
final_reordering (const hb_ot_map_t *map,
		  hb_face_t *face HB_UNUSED,
		  hb_buffer_t *buffer,
		  void *user_data HB_UNUSED)
{
  unsigned int count = buffer->len;
  if (!count) return;

  hb_mask_t init_mask = map->get_1_mask (HB_TAG('i','n','i','t'));
  hb_mask_t pref_mask = map->get_1_mask (HB_TAG('p','r','e','f'));

  hb_glyph_info_t *info = buffer->info;
  unsigned int last = 0;
  unsigned int last_syllable = info[0].syllable();
  for (unsigned int i = 1; i < count; i++)
    if (last_syllable != info[i].syllable()) {
      final_reordering_syllable (buffer, init_mask, pref_mask, last, i);
      last = i;
      last_syllable = info[last].syllable();
    }
  final_reordering_syllable (buffer, init_mask, pref_mask, last, count);

  HB_BUFFER_DEALLOCATE_VAR (buffer, indic_category);
  HB_BUFFER_DEALLOCATE_VAR (buffer, indic_position);
}



