//
// Created by james on 6/19/17.
//

#include <regex.h>
#include "utf8.h"
#include "distance.h"
#include "acdat.h"

#define MAX_WORD_DISTANCE 15
#define MAX_CHAR_DISTANCE (MAX_WORD_DISTANCE*3)


// Distance Matcher
// ========================================================


const matcher_func_l dist_matcher_func = {
    .destruct = (matcher_destruct_func) dist_destruct,
    .alloc_context = (matcher_alloc_context_func) dist_alloc_context
};

const context_func_l dist_context_func = {
    .free_context = (matcher_free_context_func) dist_free_context,
    .reset_context = (matcher_reset_context_func) dist_reset_context,
    .next = (matcher_next_func) dist_next_on_index
};

/*
 * pattern will match:
 * - A.*?B
 * - A\d{0,5}B
 * - A.{0,5}B
 */
static const char
    *pattern = "(.*)(\\.\\*\\?|(\\\\d|\\.)\\{0,([0-9]|1[0-5])\\})(.*)";
static regex_t reg;
static bool pattern_compiled = false;
static const char *tokens_delimiter = "|";

void dist_dict_before_reset(match_dict_t dict,
                            size_t *index_count,
                            size_t *buffer_size) {
  *index_count *= 2;
  *buffer_size *= 2;
}

size_t max_alternation_length(strlen_s keyword) {
  size_t max = 0;

  if ((keyword.ptr[0] == '(' && keyword.ptr[keyword.len - 1] != ')')
      || (keyword.ptr[0] != '(' && keyword.ptr[keyword.len - 1] == ')')) {
    max = keyword.len;
  } else {
    size_t i, t = 0;
    if (keyword.ptr[0] == '(' && keyword.ptr[keyword.len - 1] != ')') {
      t = 1;
    }
    size_t depth = 0, so = t;
    for (i = t; i < keyword.len - t; i++) {
      switch (keyword.ptr[i]) {
        case '|':
          if (depth == 0 && i > so) {
            size_t len = i - so;
            if (len > max) max = len;
            so = i + 1;
          }
          break;
        case '(': depth++; break;
        case ')': depth--; break;
        default: break;
      }
    }
    if (i > so) {
      size_t len = i - so;
      if (len > max) max = len;
    }
  }

  return max;
}

bool dist_dict_add_index(match_dict_t dict,
                         dict_add_indix_filter filter,
                         strlen_s keyword,
                         strlen_s extra,
                         void * tag,
                         mdi_prop_f prop) {
  int err;

  if (!pattern_compiled) {
    // compile pattern
    err = regcomp(&reg, pattern, REG_EXTENDED | REG_NEWLINE);
    if (err) {
      fprintf(stderr, "error: compile regex failed!\n");
      return false;
    } else {
      pattern_compiled = true;
    }
  }

  if (keyword.len == 0) return true;

  regmatch_t pmatch[6];
  err = regexec(&reg, keyword.ptr, 6, pmatch, 0);
  if (err == REG_NOMATCH) {
    // single
    filter->add_index(dict, filter->next, keyword, extra, tag,
                      mdi_prop_single | prop);
    return true;
  } else if (err != REG_NOERROR) {
    return false;
  }

  size_t distance;
  mdi_prop_f base_prop = mdi_prop_tag_id | mdi_prop_bufkey;
  if (pmatch[3].rm_so == -1) {
    // .*?
    distance = MAX_WORD_DISTANCE;
  } else {
    char dist[3];
    if (pmatch[4].rm_so + 1 == pmatch[4].rm_eo) {
      dist[0] = keyword.ptr[pmatch[4].rm_so];
      dist[1] = '\0';
    } else {
      dist[0] = keyword.ptr[pmatch[4].rm_so];
      dist[1] = keyword.ptr[pmatch[4].rm_so + 1];
      dist[2] = '\0';
    }
    distance = (size_t) strtol(dist, NULL, 10);

    if (keyword.ptr[pmatch[3].rm_so] == '.') {
      // .{0,n}
      ;
    } else {
      // \d{0,n}
      base_prop |= mdi_prop_dist_digit;
    }
  }

  // store original keyword
  void *key_tag = (void *) dict->idx_count;
  dict_add_index(dict, NULL, keyword, extra, tag, prop);

  // store processed keyword
  strlen_s head = {
      .ptr = keyword.ptr + pmatch[1].rm_so,
      .len = (size_t) (pmatch[1].rm_eo - pmatch[1].rm_so),
  };

  strlen_s tail = {
      .ptr = keyword.ptr + pmatch[5].rm_so,
      .len = (size_t) (pmatch[5].rm_eo - pmatch[5].rm_so),
  };

  size_t tail_max_len = max_alternation_length(tail);

  filter->add_index(dict, filter->next, head,
                    (strlen_s) {.ptr = (char *) tail_max_len, .len = 0},
                    key_tag, mdi_prop_head | base_prop);

  filter->add_index(dict, filter->next, tail,
                    (strlen_s) {.ptr = (char *) distance, .len = 0},
                    key_tag, mdi_prop_tail | base_prop);

  return true;
}

bool dist_destruct(dist_matcher_t self) {
  if (self != NULL) {
    if (self->_head_matcher != NULL) dat_destruct((datrie_t) self->_head_matcher);
    if (self->_tail_matcher != NULL) dat_destruct((datrie_t) self->_tail_matcher);
    if (self->_dict != NULL) dict_release(self->_dict);
    free(self);
    return true;
  }
  return false;
}

dist_matcher_t dist_construct_by_dict(match_dict_t dict,
                                      bool enable_automation) {
  dist_matcher_t matcher = NULL;
  trie_t head_trie = NULL;
  trie_t tail_trie = NULL;

  do {
    head_trie = trie_construct_by_dict(dict, mdi_prop_head | mdi_prop_single, false);
    if (head_trie == NULL) break;

    tail_trie = trie_construct_by_dict(dict, mdi_prop_tail, false);
    if (tail_trie == NULL) break;

    matcher = malloc(sizeof(struct dist_matcher));
    if (matcher == NULL) break;

    matcher->_dict = dict_retain(dict);

    matcher->_head_matcher = (matcher_t)
        dat_construct_by_trie(head_trie, enable_automation);
    matcher->_head_matcher->_func = dat_matcher_func;
    matcher->_head_matcher->_type =
        enable_automation ? matcher_type_acdat : matcher_type_dat;
    trie_destruct(head_trie);

    matcher->_tail_matcher = (matcher_t)
        dat_construct_by_trie(tail_trie, enable_automation);
    matcher->_tail_matcher->_func = dat_matcher_func;
    matcher->_tail_matcher->_type =
        enable_automation ? matcher_type_acdat : matcher_type_dat;
    trie_destruct(tail_trie);

    return matcher;
  } while(0);

  // clean
  trie_destruct(head_trie);
  trie_destruct(tail_trie);

  return NULL;
}

dist_matcher_t dist_construct(vocab_t vocab, bool enable_automation) {
  dist_matcher_t dist_matcher = NULL;
  match_dict_t dict = NULL;

  if (vocab == NULL) return NULL;

  dict = dict_alloc();
  if (dict == NULL) return NULL;
  dict->add_index_filter =
      dict_add_index_filter_wrap(dict->add_index_filter, dict_add_alternation_index);
  dict->add_index_filter =
      dict_add_index_filter_wrap(dict->add_index_filter, dist_dict_add_index);
  dict->before_reset = dist_dict_before_reset;

  if (dict_parse(dict, vocab)) {
    dist_matcher = dist_construct_by_dict(dict, enable_automation);
  }

  dict_release(dict);

  fprintf(stderr,
          "construct trie %s!\n",
          dist_matcher != NULL ? "success" : "failed");
  return dist_matcher;
}

dist_context_t dist_alloc_context(dist_matcher_t matcher) {
  dist_context_t ctx = NULL;

  do {
    ctx = malloc(sizeof(struct dist_context));
    if (ctx == NULL) break;

    ctx->_matcher = matcher;
    ctx->_head_context = ctx->_tail_context = ctx->_digit_context = NULL;
    ctx->_utf8_pos = NULL;

    ctx->_head_context = matcher_alloc_context(matcher->_head_matcher);
    if (ctx->_head_context == NULL) break;
    ctx->_tail_context = matcher_alloc_context(matcher->_tail_matcher);
    if (ctx->_tail_context == NULL) break;
    ctx->_digit_context = matcher_alloc_context(matcher->_tail_matcher);
    if (ctx->_digit_context == NULL) break;

    return ctx;
  } while (0);

  dist_free_context(ctx);

  return NULL;
}

bool dist_free_context(dist_context_t ctx) {
  if (ctx != NULL) {
    free(ctx->_utf8_pos);
    matcher_free_context(ctx->_tail_context);
    matcher_free_context(ctx->_head_context);
    matcher_free_context(ctx->_digit_context);
    free(ctx);
  }
  return true;
}

bool dist_reset_context(dist_context_t context, unsigned char content[],
                        size_t len) {
  if (context == NULL) return false;

  context->header.content = content;
  context->header.len = len;
  context->header.out_matched_index = NULL;
  context->header.out_e = 0;
#ifdef REPLACE_BY_ZERO
  context->_c = content[0];
#endif

  if (context->_utf8_pos != NULL) {
    free(context->_utf8_pos);
    context->_utf8_pos = NULL;
  }
  context->_utf8_pos = malloc((len + 1) * sizeof(size_t));
  if (context->_utf8_pos == NULL) {
    fprintf(stderr, "error when malloc.\n");
  }
  utf8_word_position((const char *) content, len, context->_utf8_pos);

  matcher_reset_context(context->_head_context, (char *) content, len);
  matcher_reset_context(context->_tail_context, (char *) content, len);

  context->_hcnt = 0;
  context->_htidx = 0;
  context->_state = dist_match_state_new_round;

  return false;
}

bool dist_construct_out(dist_context_t ctx, size_t _e) {
  // alias
  unsigned char *content = ctx->header.content;
  context_t hctx = ctx->_head_context;

  ctx->header.out_e = _e;

  match_dict_index_t matched_index = hctx->out_matched_index->_tag;

  ctx->header.out_matched_index = &ctx->out_index;

  ctx->out_index.length =
      ctx->header.out_e - hctx->out_e + hctx->out_matched_index->length;
#ifdef USE_SUBPATTERN_LENGTH
  // NOTE: here use sub-pattern word length.
  ctx->out_index.wlen = hctx->out_matched_index->wlen + tctx->out_matched_index->wlen;
#else
  ctx->out_index.wlen =
      utf8_word_distance(ctx->_utf8_pos,
                         hctx->out_e - hctx->out_matched_index->length,
                         ctx->header.out_e);
#endif
#ifdef REPLACE_BY_ZERO
  ctx->_c = content[ctx->header.out_e];
  content[ctx->header.out_e] = '\0';
#endif
  ctx->out_index.mdi_keyword =
      (char *) &content[hctx->out_e - hctx->out_matched_index->length];
  ctx->out_index.mdi_extra = matched_index->mdi_keyword;
  ctx->out_index._tag = matched_index->_tag;
  ctx->out_index.prop = matched_index->prop;

  return true;
}

bool dist_next_on_index(dist_context_t ctx) {
  // alias
  unsigned char *content = ctx->header.content;
  context_t hist = ctx->_history_context;
  context_t hctx = ctx->_head_context;
  context_t tctx = ctx->_tail_context;
  context_t dctx = ctx->_digit_context;

#ifdef REPLACE_BY_ZERO
  content[ctx->header.out_e] = ctx->_c;  // recover content
#endif

  switch (ctx->_state) {
    case dist_match_state_new_round: break;
    case dist_match_state_check_history: goto check_history;
    case dist_match_state_check_tail: goto check_tail;
    case dist_match_state_check_prefix: goto check_prefix;
  }

  while (dat_ac_next_on_index((dat_context_t) hctx)) {
    if (hctx->out_matched_index->prop & mdi_prop_single) {
      ctx->header.out_matched_index = hctx->out_matched_index;
      ctx->_state = dist_match_state_new_round;
      return true;
    }

    // check number
    size_t dist = (size_t) hctx->out_matched_index->mdi_extra;
    if (hctx->out_matched_index->prop & mdi_prop_dist_digit) {
      ctx->_state = dist_match_state_check_prefix;
      // skip number
      size_t tail_so = hctx->out_e;
      while (dist--) {
        tail_so++;
        if (!number_bitmap[content[tail_so]])
          break;
      }
      // check tail
      matcher_reset_context(dctx, &content[tail_so], ctx->header.len - tail_so);
check_prefix:
      while (dat_prefix_next_on_index((dat_context_t) dctx)) {
        if (dctx->out_matched_index->_tag == hctx->out_matched_index->_tag)
          return dist_construct_out(ctx, tail_so + dctx->out_e);
      }
      continue;
    }

    ctx->_state = dist_match_state_check_history;
    for (ctx->_i = (HISTORY_SIZE + ctx->_htidx - ctx->_hcnt) % HISTORY_SIZE;
         ctx->_i != ctx->_htidx; ctx->_i = (ctx->_i + 1) % HISTORY_SIZE) {
      if (hist[ctx->_i].out_e > hctx->out_e) break;
      ctx->_hcnt--;
    }
    ctx->_i--;
check_history:
    for (ctx->_i = (ctx->_i + 1) % HISTORY_SIZE; ctx->_i != ctx->_htidx;
         ctx->_i = (ctx->_i + 1) % HISTORY_SIZE) {
      long diff_pos =
          utf8_word_distance(ctx->_utf8_pos, hctx->out_e, hist[ctx->_i].out_e);
      long distance = diff_pos - hist[ctx->_i].out_matched_index->wlen;
      if (distance > MAX_WORD_DISTANCE) {  // max distance is 15
        // if diff of end_pos is longer than max_tail_length, next round.
        if (hist[ctx->_i].out_e - hctx->out_e
            > MAX_CHAR_DISTANCE + (size_t) hctx->out_matched_index->mdi_extra)
          goto next_round;
        continue;
      }

      match_dict_index_t matched_index = hist[ctx->_i].out_matched_index;
      for (; matched_index != NULL; matched_index = matched_index->_next) {
        // linked table's tag is descending order
        if (matched_index->_tag <= hctx->out_matched_index->_tag) break;
      }
      if (matched_index != NULL
          && matched_index->_tag == hctx->out_matched_index->_tag) {
        if (distance <= (size_t) matched_index->mdi_extra)
          return dist_construct_out(ctx, hist[ctx->_i].out_e);
        break;
      }
    }

    ctx->_state = dist_match_state_check_tail;
check_tail:
    // one node will match the tag only once.
    while (dat_ac_next_on_node((dat_context_t) tctx)) {
      long diff_pos =
          utf8_word_distance(ctx->_utf8_pos, hctx->out_e, tctx->out_e);
      long distance = (long) (diff_pos - tctx->out_matched_index->wlen);
      if (distance < 0) continue;

      // record history
      hist[ctx->_htidx] = *tctx;
      ctx->_htidx = (ctx->_htidx + 1) % HISTORY_SIZE;
      ctx->_hcnt++;

      if (distance > MAX_WORD_DISTANCE) {  // max distance is 15
        // if diff of end_pos is longer than max_tail_length, next round.
        if (tctx->out_e - hctx->out_e
            > MAX_CHAR_DISTANCE + (size_t) hctx->out_matched_index->mdi_extra)
          goto next_round;
        continue;
      }

      match_dict_index_t matched_index = tctx->out_matched_index;
      for (; matched_index != NULL; matched_index = matched_index->_next) {
        // linked table's tag is descending order
        if (matched_index->_tag <= hctx->out_matched_index->_tag) break;
      }
      if (matched_index != NULL
          && matched_index->_tag == hctx->out_matched_index->_tag) {
        if (distance <= (size_t) matched_index->mdi_extra)
          return dist_construct_out(ctx, tctx->out_e);
        break;
      }
    }
next_round:
    ctx->_state = dist_match_state_new_round;
  }

  return false;
}
