#include "counters.h"
#include "flags.h"
#include "logging.h"
#include "params.h"

#include <assert.h>
#include <cctype>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

DEFINE_PARAM(prefer_sharp, 0, "When branching, prefer items with a # prefix");

DEFINE_PARAM(prefer_unsharp, 0,
             "When branching, prefer items without a # prefix");

struct Node {
  std::string name;
  size_t ulink;
  size_t dlink;
  size_t llink;
  size_t rlink;
  size_t slack;
  size_t bound;
  int top_or_len;
  int color = 0;
};

#define NAME(i) (nodes[i].name)
#define LLINK(i) (nodes[i].llink)
#define RLINK(i) (nodes[i].rlink)
#define ULINK(i) (nodes[i].ulink)
#define DLINK(i) (nodes[i].dlink)
#define TOP(i) (nodes[i].top_or_len)
#define LEN(i) (nodes[i].top_or_len)
#define COLOR(i) (nodes[i].color)
#define SLACK(i) (nodes[i].slack)
#define BOUND(i) (nodes[i].bound)
#define WEIGHT(i) (nodes[i].rlink)
#define REMAINING_WEIGHT(i) (nodes[i].top_or_len)
#define HAS_WEIGHTED_OPTIONS(i) (nodes[i].color)
#define OPTION_ROW(i) (nodes[i].llink)
#define MAX_LINE_SIZE (100000)

inline size_t monus(size_t x, size_t y) { return x > y ? x - y : 0; }

struct MCC {
  std::vector<Node> nodes;
  std::vector<size_t> choice;
  std::vector<size_t> ft;
  std::vector<size_t> score;
  std::vector<std::string> colors; // 1-indexed.
  std::vector<bool>
      option_legal; // Buffer to track legal options during choose_item
  size_t num_items;
  size_t num_primary_items;
  size_t num_options;

  std::string debug_nodes() {
    std::ostringstream oss;
    oss << std::endl;
    int i = 0;
    for (const Node &n : nodes) {
      oss << i << ": { " << n.name << " l: " << n.llink << " r: " << n.rlink
          << " u: " << n.ulink << " d: " << n.dlink << " t: " << n.top_or_len
          << " c: " << n.color << " s: " << n.slack << " b: " << n.bound
          << " w: " << n.color << " }" << std::endl;
      ++i;
    }
    return oss.str();
  }

  int color_parse(char *ss, std::unordered_map<std::string, int> &colors,
                  int next_color, std::string *curr) {
    for (size_t i = 0; ss[i] != 0; ++i) {
      if (ss[i] == ':') {
        *curr = std::string(ss, i);
        std::string color(&ss[i + 1]);
        CHECK(color.size() > 0) << "Bad color spec: " << ss;
        if (colors[color] == 0) {
          colors[color] = next_color;
        }
        return colors[color];
      }
    }
    *curr = ss;
    return 0;
  }

  std::tuple<int, bool> weight_parse(char *ss, std::string *curr) {
    for (size_t i = 0; ss[i] != 0; ++i) {
      if (ss[i] == '=') {
        *curr = std::string(ss, i);
        std::string weight_s(&ss[i + 1]);
        int weight = std::stoi(weight_s);
        CHECK(weight >= 1) << "Bad weight: " << weight;
        return {weight, true};
      }
    }
    *curr = ss;
    return {1, false};
  }

  void multiplicity_parse(std::string *curr, size_t *low, size_t *high) {
    size_t curr_size = curr->size();
    size_t state = 0;
    for (size_t i = 0; i < curr->size(); ++i) {
      char ch = (*curr)[i];
      if (state == 0 && ch == '[') {
        curr_size = i;
        ++state;
        *low = 0;
      } else if (state == 1) {
        CHECK(ch == ':' || isdigit(ch))
            << "Expected ':' or digit after '[': " << *curr;
        if (ch == ':') {
          ++state;
          *high = 0;
        } else /* isdigit */ {
          *low *= 10;
          *low += ch - '0';
        }
      } else if (state == 2) {
        CHECK(ch == ']' || isdigit(ch))
            << "Expected ']' or digit after ':': " << *curr;
        if (ch == ']') {
          ++state;
        } else /* isdigit */ {
          *high *= 10;
          *high += ch - '0';
        }
      } else if (state == 3) {
        CHECK(false) << "Expected no more input after ']': " << *curr;
      }
    }
    CHECK(*low >= 0) << "Bad low bound (" << *low << ") for " << *curr;
    CHECK(*high >= 1) << "Bad high bound (" << *high << ") for " << *curr;
    CHECK(state == 0 || state == 3)
        << "Incomplete input for option with multiplicity: " << *curr;
    *curr = curr->substr(0, curr_size);
  }

  MCC(const char *filename) {
    FILE *f = fopen(filename, "r");
    CHECK(f) << "Failed to open file: " << filename;
    char s[MAX_LINE_SIZE];
    char ss[MAX_LINE_SIZE];

    // I1. [Read the first line.]
    std::unordered_map<std::string, size_t> header;
    nodes.push_back(Node()); // Header
    num_primary_items = std::numeric_limits<size_t>::max();
    while (fgets(s, MAX_LINE_SIZE, f) != NULL) {
      CHECK(strlen(s) < MAX_LINE_SIZE - 1 || s[strlen(s) - 1] == '\n')
          << "Input line too long. Recompile with larger MAX_LINE_SIZE.";
      int offset = 0, r = 0;
      std::string curr;
      while (sscanf(s + offset, " %s %n", ss, &r) > 0) {
        curr = ss;
        if (curr[0] == '/' && curr.size() > 1 && curr[1] == '/')
          break;
        if (curr == "\\")
          break;
        offset += r;
        size_t low = 1, high = 1;
        multiplicity_parse(&curr, &low, &high);
        CHECK(header.find(curr) == header.end())
            << "Duplicate item name: '" << ss << "'";
        if (curr == "|") {
          num_primary_items = nodes.size() - 1;
          continue;
        }
        header[curr] = nodes.size();
        Node n;
        n.name = curr;
        n.llink = nodes.size() - 1;
        n.slack = high - low;
        n.bound = high;
        nodes.back().rlink = nodes.size();
        nodes.push_back(n);
      }
      if (curr != "\\" && !header.empty())
        break;
    }
    num_items = header.size();

    // I2. [Finish the horizontal list.]
    if (num_primary_items == std::numeric_limits<size_t>::max()) {
      num_primary_items = nodes.size() - 1;
    } else {
      nodes[num_primary_items + 1].llink = nodes.size() - 1;
      nodes.back().rlink = num_primary_items + 1;
    }
    nodes[num_primary_items].rlink = 0;
    nodes[0].llink = num_primary_items;
    LOG(1) << "Parsed " << num_items << " items (" << num_primary_items
           << " primary)";

    // I3. [Prepare for options.]
    for (size_t i = 1; i < nodes.size(); ++i) {
      REMAINING_WEIGHT(i) = 0;
      ULINK(i) = DLINK(i) = i;
    }
    size_t m = 0;
    size_t p = nodes.size();
    nodes.push_back(Node()); // First spacer

    num_options = 0;
    std::unordered_set<std::string> seen;
    std::unordered_map<std::string, int> color_ids;
    int next_color = 1;
    size_t j = 0;

    while (fgets(s, MAX_LINE_SIZE, f) != NULL) {
      CHECK(strlen(s) < MAX_LINE_SIZE - 1 || s[strlen(s) - 1] == '\n')
          << "Input line too long. Recompile with larger MAX_LINE_SIZE.";
      int offset = 0, r = 0, cnum = 0;
      std::string curr;
      while (sscanf(s + offset, " %s %n", ss, &r) > 0) {
        auto [weight, parsed_weight] = weight_parse(ss, &curr);
        LOG(2) << "Parsed weight: " << weight;

        // Don't attempt parsing color if a weight was parsed, otherwise
        // color_parse will reset the curr to ss and we'll think that "a=w" is
        // the name of the item, leading to an error.
        if (parsed_weight) {
          cnum = 0;
        } else {
          cnum = color_parse(ss, color_ids, next_color, &curr);
        }

        next_color = std::max(next_color, cnum + 1);

        if (curr[0] == '/' && curr.size() > 1 && curr[1] == '/')
          break;
        if (curr == "\\")
          break;
        offset += r;
        ++j;
        size_t i = header[curr];
        CHECK(i > 0) << "Item '" << curr << "' not in header";
        CHECK(i >= num_primary_items || cnum == 0)
            << "Color can't be assigned to primary item (" << ss << ")";
        CHECK(seen.find(curr) == seen.end())
            << "Duplicate item '" << curr << "'";
        seen.insert(curr);

        CHECK(weight >= 1) << "Bad weight: " << weight;
        CHECK(i <= num_primary_items || weight == 1)
            << "Secondary items cannot have weighted options (" << ss << ")";

        // For each item, weight = sum of weights of options.
        REMAINING_WEIGHT(i) += weight;
        if (weight > 1) {
          HAS_WEIGHTED_OPTIONS(i) = true;
        }

        size_t q = ULINK(i);
        nodes.push_back(Node());
        CHECK(nodes.size() > p + j) << "Not enough nodes allocated. Want "
                                    << p + j << ", got " << nodes.size() - 1;
        ULINK(p + j) = q;
        DLINK(q) = p + j;
        DLINK(p + j) = i;
        ULINK(i) = p + j;
        TOP(p + j) = i;
        if (i <= num_primary_items) {
          WEIGHT(p + j) = weight;
        } else {
          COLOR(p + j) = cnum;
        }
        OPTION_ROW(p + j) = m; // Store which option row this node belongs to
      }
      if (curr == "\\" || seen.empty())
        continue;

      // I5. [Finish an option.]
      num_options++;
      m++;
      DLINK(p) = p + j;
      nodes.push_back(Node());
      CHECK(nodes.size() - 1 == p + j + 1) << "No room for spacer";
      p = p + j + 1;
      TOP(p) = -m;
      ULINK(p) = p - j;
      seen.clear();
      j = 0;
    }

    LOG(1) << "Parsed " << color_ids.size() << " colors";
    LOG(1) << "Parsed " << num_options << " options";

    // Sort options by descending weight for each item
    for (size_t i = 1; i <= num_items; ++i) {
      // Collect all option nodes for this item
      std::vector<size_t> option_nodes;
      for (size_t p = DLINK(i); p != i; p = DLINK(p)) {
        option_nodes.push_back(p);
      }

      // Skip items with 0 or 1 options (nothing to sort)
      if (option_nodes.size() <= 1)
        continue;

      // Sort by weight (descending)
      std::sort(option_nodes.begin(), option_nodes.end(),
                [this](size_t a, size_t b) { return WEIGHT(a) > WEIGHT(b); });

      // Rebuild the circular doubly-linked list in sorted order
      DLINK(i) = option_nodes[0];
      ULINK(option_nodes[0]) = i;

      for (size_t j = 0; j < option_nodes.size(); ++j) {
        size_t curr = option_nodes[j];
        size_t next = (j + 1 < option_nodes.size()) ? option_nodes[j + 1] : i;
        DLINK(curr) = next;
        ULINK(next) = curr;
      }
    }
    LOG(3) << "After parsing, memory is: " << debug_nodes();
    fclose(f);

    choice = std::vector<size_t>(num_options);
    ft = std::vector<size_t>(num_options);
    score = std::vector<size_t>(num_options);
    option_legal = std::vector<bool>(num_options); // Sized to number of options
    colors.resize(color_ids.size() + 1);           // colors are 1-indexed.
    for (const auto &kv : color_ids) {
      colors[kv.second] = kv.first;
    }
  }

  // p: an option node
  // removes every node in this option from its vertical item list, EXCEPT p
  void hide(size_t p) {
    for (size_t q = p + 1; q != p; ++q) {
      if (COLOR(q) < 0)
        continue;
      int x = TOP(q);
      size_t u = ULINK(q), d = DLINK(q);
      if (x <= 0) {
        q = u - 1;
        continue;
      } // q was a spacer.
      DLINK(u) = d;
      ULINK(d) = u;
      // q is an option node, x is its item.
      assert(REMAINING_WEIGHT(x) >= WEIGHT(q));
      REMAINING_WEIGHT(x) -= WEIGHT(q);
      // assert(LEN(x) >= 1);
      // LEN(x) -= 1;
    }
  }

  // p: an option node
  // re-inserts every node in this option into its vertical item list, EXCEPT p,
  // which wasn't removed.
  void unhide(size_t p) {
    for (size_t q = p - 1; q != p; --q) {
      if (COLOR(q) < 0)
        continue;
      int x = TOP(q);
      size_t u = ULINK(q), d = DLINK(q);
      if (x <= 0) {
        q = d + 1;
        continue;
      } // q was a spacer.
      DLINK(u) = q;
      ULINK(d) = q;
      REMAINING_WEIGHT(x) += WEIGHT(q);
      // LEN(x) += 1;
    }
  }

  void cover(size_t i) {
    for (size_t p = DLINK(i); p != i; p = DLINK(p)) {
      hide(p);
    }
    size_t l = LLINK(i), r = RLINK(i);
    RLINK(l) = r;
    LLINK(r) = l;
  }

  void uncover(size_t i) {
    size_t l = LLINK(i), r = RLINK(i);
    RLINK(l) = i;
    LLINK(r) = i;
    for (size_t p = ULINK(i); p != i; p = ULINK(p)) {
      unhide(p);
    }
  }

  void purify(size_t p) {
    int c = COLOR(p), i = TOP(p);
    CHECK(i >= 0) << "Bad top value for " << p;
    COLOR(i) = c;
    for (size_t q = DLINK(i); q != static_cast<size_t>(i); q = DLINK(q)) {
      if (COLOR(q) == c) {
        COLOR(q) = -1;
      } else {
        hide(q);
      }
    }
  }

  void unpurify(size_t p) {
    int c = COLOR(p), i = TOP(p);
    CHECK(i >= 0) << "Bad top value for " << p;
    for (size_t q = ULINK(i); q != static_cast<size_t>(i); q = ULINK(q)) {
      if (COLOR(q) < 0) {
        COLOR(q) = c;
      } else {
        unhide(q);
      }
    }
  }

  void commit(size_t p, size_t j) {
    if (COLOR(p) == 0)
      cover(j);
    if (COLOR(p) > 0)
      purify(p);
  }

  void uncommit(size_t p, size_t j) {
    if (COLOR(p) == 0)
      uncover(j);
    if (COLOR(p) > 0)
      unpurify(p);
  }

  // x: an option node
  // p: an item node
  void tweak(size_t x, size_t p) {
    CHECK(p <= num_items);
    CHECK(p <= num_primary_items);
    CHECK(x == DLINK(p));
    CHECK(p == ULINK(x));
    CHECK(COLOR(x) >= 0) << "Attempt to tweak non-primary?";
    if (BOUND(p) != 0)
      hide(x);
    size_t d = DLINK(x);
    DLINK(p) = d;
    ULINK(d) = p;
    assert(REMAINING_WEIGHT(p) >= WEIGHT(x));
    REMAINING_WEIGHT(p) -= WEIGHT(x);
    // assert(LEN(p) >= 1);
    // LEN(p) -= 1;
  }

  void untweak(size_t a, size_t i) {
    bool special = BOUND(i) == 0;
    int p = a <= num_items ? a : TOP(a);
    size_t x = a, y = p;
    size_t z = DLINK(p);
    DLINK(p) = x;
    size_t k = 0;
    // size_t num_items_added = 0;
    while (x != z) {
      ULINK(x) = y;
      // ++k;
      k += WEIGHT(x);
      // num_items_added++;
      if (!special)
        unhide(x);
      y = x;
      x = DLINK(x);
    }
    ULINK(z) = y;
    REMAINING_WEIGHT(p) += k;
    // LEN(p) += num_items_added;
    if (special)
      uncover(p);
  }
  // x: an option node
  bool can_try_option(size_t x) {
    size_t p = x;

    do {
      size_t j = TOP(p);
      if (TOP(p) <= 0) {
        // p was a spacer
        p = ULINK(p) - 1;
      } else if (j <= num_primary_items) {
        if (BOUND(j) < WEIGHT(p)) {
          return false;
        }
      }
      ++p;
    } while (p != x);

    return true;
  }

  void tweak_illegal_options(size_t l, size_t i) {
    // LOG(2) << "Tweaking illegal options. choice[" << l << "] = " << choice[l]
    //        << ", i = " << i;
    // int k = 0;
    while (choice[l] != i && !can_try_option(choice[l])) {
      // k++;
      // LOG(2) << "Tweaking illegal option " << choice[l];
      tweak(choice[l], i);
      choice[l] = DLINK(choice[l]);
    }
    // LOG(2) << "Tweaked " << k << " illegal options.";
  }

  // x: an option node
  void try_option(size_t x) {
    // LOG(2) << "try_option called on node " << x;

    size_t p = x;

    do {
      // p: another option node in the same option as x
      size_t j = TOP(p);
      if (TOP(p) <= 0) {
        p = ULINK(p) - 1;
      } else if (j <= num_primary_items) {
        // auto old_bound = BOUND(j);

        assert(BOUND(j) >= WEIGHT(p));
        BOUND(j) -= WEIGHT(p);

        // LOG(2) << "try option working on node " << p << " with WEIGHT "
        //        << WEIGHT(p) << ", its top is " << j << " with old BOUND "
        //        << old_bound << ", new bound " << BOUND(j);

        if (BOUND(j) == 0)
          cover(j);

      } else {
        commit(p, j);
      }

      ++p;
    } while (p != x);
  }

  void try_again(size_t x) {
    size_t p = x - 1;
    do {
      size_t j = TOP(p);
      if (TOP(p) <= 0) {
        p = DLINK(p) + 1;
      } else if (j <= num_primary_items) {
        // ++BOUND(j);
        int old_bound = BOUND(j);
        BOUND(j) += WEIGHT(p);
        if (old_bound == 0) {
          assert(BOUND(j) >= 1);
          uncover(j);
        }
      } else {
        uncommit(p, j);
      }

      assert(p > 0);
      --p;
    } while (p != (x - 1));
  }

  // Returns true iff backtracking was successful.
  bool backtrack(size_t &l, size_t &i) {
    while (true) {
      // M9. [Leave level l.]
      if (l == 0)
        return false;
      assert(l > 0);
      --l;
      if (choice[l] <= num_items) {
        i = choice[l];
        size_t p = LLINK(i);
        size_t q = RLINK(i);
        LLINK(q) = i;
        RLINK(p) = i;

        // M8. [Restore i.]
        if (BOUND(i) == 0 && SLACK(i) == 0)
          uncover(i);
        else
          untweak(ft[l], i);
        // TODO where's the --BOUND(p) corresponding to this?
        // ++BOUND(i); // -> M9

      } else {
        i = TOP(choice[l]);
        CHECK(static_cast<int>(i) == TOP(choice[l]));

        // M7. [Try again.]
        try_again(choice[l]);
        choice[l] = DLINK(choice[l]);
        return true; // -> M5
      }
    }
  }

  /*
  Maybe for illegal options we should tweak them but then return false?
  NO.
  When should_try returns false that means we've tried everything. We uncover
  and untweak.
  So maybe should_try should tweak in a loop? Until it's exhausted every option,
  and then return false? Yes.
  */
  /*
  TODO: the current question is: when we eventually get to the choice[l] == i
  case, and we're under bound, what is supposed to happen?

  Answer: the actual choices to put us in a satisfying state have happened above
  us. If BOUND(i) == 0 then we would have covered before getting here. If we are
  not in a satisfying state then the "return false" below will fire.

  So if there are no more options to try, and we're in a satisfying state, and
  we haven't already covered, then we should cover here because this item is no
  longer active.
  */
  /*
  If choice[l] = i i.e. no more options to try, then:
   * if currently satisfying then cover item
   * if currently not satisfying then don't cover and return false
  */
  bool should_try(size_t l, size_t i) {
    /*
    Really we should split this into two cases:
    1. There are no more options to try: either we're satisfying or not
    2. There are more options to try. Figure out if we can ever satisfy

    The semantics of BOUND are: it's the total remaining BOUND on i *before*
    trying any options at this level.
    */
    // M5. [Possibly tweak x_l.]
    // LOG(2) << "should try: l = " << l << ", i = " << i
    //        << ", choice[l] = " << choice[l] << ", BOUND(i) = " << BOUND(i)
    //        << ", SLACK(i) = " << SLACK(i)
    //        << ", REMAINING_WEIGHT(i) = " << REMAINING_WEIGHT(i);

    assert(i <= num_items);
    assert(i <= num_primary_items);

    if (choice[l] == i) {
      // LOG(2) << "No more options to try";

      if (BOUND(i) >= 0 && SLACK(i) >= BOUND(i)) {
        /* Currently within limits; we should deactivate this item and
         * potentially visit the solution if there are no more items. */
        size_t p = LLINK(i);
        size_t q = RLINK(i);
        RLINK(p) = q;
        LLINK(q) = p;
        return true;
      } else {
        /* Currently not within limits, and since there are no more options, we
         * never will be. Do not deactivate this item, and therefore do not
         * visit solutions. */
        return false;
      }
    } else {
      // LOG(2) << "Yes more options to try (including this one).";

      assert(TOP(choice[l]) == i);
      assert(WEIGHT(choice[l]) <= BOUND(i));

      int remaining_bound = (int)BOUND(i) - (int)WEIGHT(choice[l]);
      int remaining_options_weight =
          (int)REMAINING_WEIGHT(i) - (int)WEIGHT(choice[l]);

      if (remaining_bound - remaining_options_weight > (int)SLACK(i)) {
        /* Not enough remaining weight; abort this branch. */
        // LOG(2) << "Not enough remaining weight; abort this branch. Remaining
        // "
        //           "bound "
        //        << remaining_bound << ", remaining options weight "
        //        << remaining_options_weight << ", SLACK " << SLACK(i);
        return false;
      } else {
        /* We have more options, and there's enough remaining weight on them
         * that we could possibly end up within limits. Deactivate this option;
         * the calling function will then include it in the solution (hide all
         * conflicting options) and potentially visit solutions.
         */
        // LOG(2) << "We have more options and there's enough remaining weight
        // on "
        //           "them.";
        tweak(choice[l], i);
        return true;
      }
    }
  }

  size_t choose_item(size_t l) {
    int best_branch_factor = std::numeric_limits<int>::max();
    size_t i = RLINK(0);
    INC(choices);

    // Pass 1: Mark all options as legal or illegal based on bound constraints
    std::fill(option_legal.begin(), option_legal.end(), true);

    for (size_t p = RLINK(0); p != 0; p = RLINK(p)) {
      // For each item, go through its option nodes and mark options that
      // violate this item's bound
      for (size_t o = DLINK(p); o != p; o = DLINK(o)) {
        if (BOUND(p) < WEIGHT(o)) {
          // This option node violates the bound constraint on item p
          option_legal[OPTION_ROW(o)] = false;
        } else {
          // The remaining options must be legal, because options are sorted in
          // descending order within each item.
          break;
        }
      }
    }

    // Pass 2: Count branch factors, skipping illegal options
    for (size_t p = RLINK(0); p != 0; p = RLINK(p)) {
      int branch_factor = 0;

      if (!HAS_WEIGHTED_OPTIONS(p)) {
        // Count legal options
        int legal_count = 0;
        for (size_t o = DLINK(p); o != p; o = DLINK(o)) {
          if (option_legal[OPTION_ROW(o)]) {
            legal_count++;
          }
        }
        branch_factor = monus(legal_count + 1, monus(BOUND(p), SLACK(p)));
      } else {

        // Count how many options we'll actually try before backtracking
        // We try options in order until remaining weight < remaining bound
        int remaining_weight = REMAINING_WEIGHT(p);
        int remaining_bound = BOUND(p);

        for (size_t o = DLINK(p); o != p; o = DLINK(o)) {
          // Skip illegal options
          if (!option_legal[OPTION_ROW(o)]) {
            // Still update running totals
            remaining_weight -= WEIGHT(o);
            continue;
          }

          // Can we try this option?
          if (WEIGHT(o) <= remaining_bound) {
            // After trying this option, will there be enough weight left
            // to possibly satisfy the remaining bound?
            int weight_after = remaining_weight - WEIGHT(o);
            int bound_after = remaining_bound - WEIGHT(o);

            // We can try this option if:
            // bound_after - weight_after <= SLACK(p)
            // i.e., we're not too far below the lower bound
            if (bound_after - weight_after <= (int)SLACK(p)) {
              branch_factor += 1;
            }
          }
          // Update running totals for suffix weight calculation
          remaining_weight -= WEIGHT(o);
        }

        // Add 1 for the null branch if we're already satisfied
        if (BOUND(p) <= SLACK(p) && BOUND(p) >= 0) {
          branch_factor += 1;
        }
      }

      int s = branch_factor;
      if ((PARAM_prefer_sharp && s > 1 && NAME(p)[0] != '#') ||
          (PARAM_prefer_unsharp && s > 1 && NAME(p)[0] == '#')) {
        s += num_options;
      }

      if (s < best_branch_factor ||
          (s == best_branch_factor && SLACK(p) < SLACK(i)) ||
          (s == best_branch_factor && SLACK(p) == SLACK(i) &&
           REMAINING_WEIGHT(p) > REMAINING_WEIGHT(i))) {
        best_branch_factor = s;
        i = p;
      }

      if (s == 0) {
        break;
      }
    }
    INC(score, best_branch_factor);
    score[l] = best_branch_factor;
    ft[l] = 0;
    LOG(2) << "Chose i=" << i << " (" << NAME(i) << ")";
    return i;
  }

  void visit(size_t l) {
    if (LOG_ENABLED(1)) {
      std::ostringstream oss;
      oss << "Solution: " << std::endl;
      for (size_t j = 0; j < l; ++j) {
        size_t r = choice[j];
        if (r <= num_items)
          continue;
        while (TOP(r) >= 0)
          ++r;
        oss << "  " << -TOP(r) << ": ";
        for (size_t p = ULINK(r); TOP(p) > 0; ++p) {
          size_t q = TOP(p);
          oss << NAME(q);
          if (COLOR(q) > 0)
            oss << ":" << colors[COLOR(q)];
          if (WEIGHT(p) != 1) {
            oss << "=" << WEIGHT(p);
          }
          oss << " ";
        }
        oss << std::endl;
      }
      LOG(1) << oss.str();
    }
  }

  double progress(size_t l) {
    double p = 0;
    double denom = 1;
    for (size_t j = 0; j < l; ++j) {
      size_t i = choice[j];
      size_t c = i <= num_items ? i : TOP(i);
      size_t k = 1;
      for (size_t q = ft[j] ? ft[j] : DLINK(c); q != choice[j]; q = DLINK(q)) {
        ++k;
      }
      denom *= score[j];
      p += (k - 1) / denom;
    }
    p += 1.0 / (2.0 * denom);
    return p * 100;
  }

  void solve_() {
    // M1. [Initialize.]
    INITCOUNTER(solutions);
    size_t l = 0;

    while (true) {
      // M3. [Choose i.]
      size_t i = choose_item(l);
      if (score[l] == 0) {
        INC(score_zero);
        if (!backtrack(l, i))
          return;
      } else {
        // M4. [Prepare to branch on i.]
        choice[l] = DLINK(i);
        /*
        Two major problems:

        1. When to cover.
        Previously, once you'd chosen i, you knew whether or not to cover.
        Because whatever option you took, you knew that you'd go down to BOUND =
        0 or not.

        Now, simply choosing i doesn't mean you know whether you need to cover.

        Different options may or may not end up causing us to hit BOUND = 0.

        2. Illegal options i.e. those that would cause an item to go over
        bounds.
          */
        // if (--BOUND(i) == 0)
        //   cover(i);
        if (BOUND(i) != 0 || SLACK(i) != 0)
          ft[l] = choice[l];
      }

      while (true) {
        // LOG_EVERY_N_SECS_T(0, 1)
        //     << "sols: " << GETCOUNTER(solutions)
        //     << " done: " << std::setprecision(3) << progress(l) << "%";

        // We could simply try to have should_try return false if anything would
        // go over BOUND? Maybe another way to think of it is that we simply DO
        // try on the illegal option, then backtrack.
        // Illegal options must be tweaked, so they won't be tried later.
        // Right now tweaking happens in should_try.

        tweak_illegal_options(l, i);

        if (should_try(l, i)) {
          // M6. [Try x_l.]
          LOG(2) << "Trying x_" << l << " = " << choice[l];
          if (choice[l] != i)
            try_option(choice[l]);
          ++l;

          // M2. [Enter level l.]
          if (RLINK(0) != 0)
            break; // -> M3
          assert(RLINK(0) == 0);
          INC(solutions);
          visit(l);
        } else {
          // M8. [Restore i.]
          if (BOUND(i) == 0 && SLACK(i) == 0)
            uncover(i);
          else
            untweak(ft[l], i);
          // ++BOUND(i);
        }

        if (!backtrack(l, i))
          return;
      }
    }
  }

  void solve() {
    std::string memory_before = debug_nodes();
    solve_();
    LOG(2) << "At the end, memory is " << debug_nodes();
    std::string memory_after = debug_nodes();

    if (memory_before != memory_after) {
      LOG(2) << "memory different!";
    } else {
      LOG(2) << "Memory is same before and after full search";
    }
  }
};

int main(int argc, char **argv) {
  int oidx;
  CHECK(parse_flags(argc, argv, &oidx))
      << "Usage: " << argv[0] << " [-vV] <filename>\n"
      << "V: verbosity (>= 2 prints solutions)\n";
  CHECK(!PARAM_prefer_sharp || !PARAM_prefer_unsharp)
      << "Both prefer_sharp and prefer_unsharp are set. Use only one.";
  init_counters();
  MCC(argv[oidx]).solve();
  return 0;
}
