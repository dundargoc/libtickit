#include "tickit.h"
#include "hooklists.h"

#include <stdio.h>

#define ROOT_AS_WINDOW(root) ((TickitWindow*)root)
#define WINDOW_AS_ROOT(win)  ((TickitRootWindow*)win)

#define DEBUG_LOGF  if(tickit_debug_enabled) tickit_debug_logf

typedef enum {
  TICKIT_HIERARCHY_INSERT_FIRST,
  TICKIT_HIERARCHY_INSERT_LAST,
  TICKIT_HIERARCHY_REMOVE,
  TICKIT_HIERARCHY_RAISE,
  TICKIT_HIERARCHY_RAISE_FRONT,
  TICKIT_HIERARCHY_LOWER,
  TICKIT_HIERARCHY_LOWER_BACK
} HierarchyChangeType;

struct TickitWindow {
  TickitWindow *parent;
  TickitWindow *first_child;
  TickitWindow *next;
  TickitWindow *focused_child;
  TickitPen *pen;
  TickitRect rect;
  struct {
    int line;
    int col;
    TickitCursorShape shape;
    bool visible;
  } cursor;
  bool is_visible;
  bool is_focused;
  bool steal_input;
  bool focus_child_notify;

  struct TickitEventHook *hooks;
};

#define WINDOW_PRINTF_FMT     "[%dx%d abs@%d,%d]"
#define WINDOW_PRINTF_ARGS(w) (w)->rect.cols, (w)->rect.lines, tickit_window_get_abs_geometry(w).left, tickit_window_get_abs_geometry(w).top

#define RECT_PRINTF_FMT     "[(%d,%d)..(%d,%d)]"
#define RECT_PRINTF_ARGS(r) (r).left, (r).top, tickit_rect_right(&(r)), tickit_rect_bottom(&(r))

DEFINE_HOOKLIST_FUNCS(window,TickitWindow,TickitWindowEventFn)

typedef struct HierarchyChange HierarchyChange;
struct HierarchyChange {
  HierarchyChangeType change;
  TickitWindow *parent;
  TickitWindow *win;
  HierarchyChange *next;
};

typedef struct TickitRootWindow TickitRootWindow;
struct TickitRootWindow {
  TickitWindow win;

  TickitTerm *term;
  TickitRectSet *damage;
  HierarchyChange *hierarchy_changes;
  bool needs_expose;
  bool needs_restore;
  bool needs_later_processing;

  int event_id;

  // Drag/drop context handling
  bool mouse_dragging;
  int mouse_last_button;
  int mouse_last_line;
  int mouse_last_col;
  TickitWindow *drag_source_window;
};

static void _request_restore(TickitRootWindow *root);
static void _request_later_processing(TickitRootWindow *root);
static void _request_hierarchy_change(HierarchyChangeType, TickitWindow *);
static void _do_hierarchy_change(HierarchyChangeType change, TickitWindow *parent, TickitWindow *win);
static void _purge_hierarchy_changes(TickitWindow *win);
static int _handle_key(TickitWindow *win, TickitKeyEventInfo *args);
static TickitWindow *_handle_mouse(TickitWindow *win, TickitMouseEventInfo *args);

static int on_term(TickitTerm *term, TickitEventType ev, void *_info, void *user)
{
  TickitRootWindow *root = user;
  TickitWindow *win = ROOT_AS_WINDOW(root);

  if(ev & TICKIT_EV_RESIZE) {
    TickitResizeEventInfo *info = _info;
    int oldlines = win->rect.lines;
    int oldcols  = win->rect.cols;

    tickit_window_resize(win, info->lines, info->cols);
    DEBUG_LOGF("Ir", "Resize to %dx%d",
        info->cols, info->lines);

    if(info->lines > oldlines) {
      TickitRect damage = {
        .top   = oldlines,
        .left  = 0,
        .lines = info->lines - oldlines,
        .cols  = info->cols,
      };
      tickit_window_expose(win, &damage);
    }

    if(info->cols > oldcols) {
      TickitRect damage = {
        .top   = 0,
        .left  = oldcols,
        .lines = oldlines,
        .cols  = info->cols - oldcols,
      };
      tickit_window_expose(win, &damage);
    }
  }

  if(ev & TICKIT_EV_KEY) {
    TickitKeyEventInfo *info = _info;
    static const char * const evnames[] = { NULL, "KEY", "TEXT" };

    DEBUG_LOGF("Ik", "Key event %s %s (mod=%02x)",
        evnames[info->type], info->str, info->mod);

    _handle_key(win, info);
  }

  if(ev & TICKIT_EV_MOUSE) {
    TickitMouseEventInfo *info = _info;
    static const char * const evnames[] = { NULL, "PRESS", "DRAG", "RELEASE", "WHEEL" };

    DEBUG_LOGF("Im", "Mouse event %s %d @%d,%d (mod=%02x)",
        evnames[info->type], info->button, info->col, info->line, info->mod);

    if(info->type == TICKIT_MOUSEEV_PRESS) {
      /* Save the last press location in case of drag */
      root->mouse_last_button = info->button;
      root->mouse_last_line   = info->line;
      root->mouse_last_col    = info->col;
    }
    else if(info->type == TICKIT_MOUSEEV_DRAG && !root->mouse_dragging) {
      TickitMouseEventInfo draginfo = {
        .type   = TICKIT_MOUSEEV_DRAG_START,
        .button = root->mouse_last_button,
        .line   = root->mouse_last_line,
        .col    = root->mouse_last_col,
      };

      root->drag_source_window = _handle_mouse(win, &draginfo);
      root->mouse_dragging = true;
    }
    else if(info->type == TICKIT_MOUSEEV_RELEASE && root->mouse_dragging) {
      TickitMouseEventInfo draginfo = {
        .type   = TICKIT_MOUSEEV_DRAG_DROP,
        .button = info->button,
        .line   = info->line,
        .col    = info->col,
      };

      _handle_mouse(win, &draginfo);

      if(root->drag_source_window) {
        TickitRect geom = tickit_window_get_abs_geometry(root->drag_source_window);
        TickitMouseEventInfo draginfo = {
          .type   = TICKIT_MOUSEEV_DRAG_STOP,
          .button = info->button,
          .line   = info->line - geom.top,
          .col    = info->col  - geom.left,
        };

        _handle_mouse(root->drag_source_window, &draginfo);
      }

      root->mouse_dragging = false;
    }

    TickitWindow *handled = _handle_mouse(win, info);

    if(info->type == TICKIT_MOUSEEV_DRAG &&
       root->drag_source_window &&
       (!handled || handled != root->drag_source_window)) {
      TickitRect geom = tickit_window_get_abs_geometry(root->drag_source_window);
      TickitMouseEventInfo draginfo = {
        .type   = TICKIT_MOUSEEV_DRAG_OUTSIDE,
        .button = info->button,
        .line   = info->line - geom.top,
        .col    = info->col  - geom.left,
      };

      _handle_mouse(root->drag_source_window, &draginfo);
    }
  }

  return 1;
}

static void init_window(TickitWindow *win, TickitWindow *parent, TickitRect rect)
{
  win->parent = parent;
  win->first_child = NULL;
  win->next = NULL;
  win->focused_child = NULL;
  win->pen = tickit_pen_new();
  win->rect = rect;
  win->cursor.line = 0;
  win->cursor.col = 0;
  win->cursor.shape = TICKIT_CURSORSHAPE_BLOCK;
  win->cursor.visible = true;
  win->is_visible = true;
  win->is_focused = false;
  win->steal_input = false;
  win->focus_child_notify = false;

  win->hooks = NULL;
}

TickitWindow* tickit_window_new_root(TickitTerm *term)
{
  int lines, cols;
  tickit_term_get_size(term, &lines, &cols);

  TickitRootWindow *root = malloc(sizeof(TickitRootWindow));
  if(!root)
    return NULL;

  init_window(ROOT_AS_WINDOW(root), NULL, (TickitRect) { .top = 0, .left = 0, .lines = lines, .cols = cols });

  root->term = term;
  root->hierarchy_changes = NULL;
  root->needs_expose = false;
  root->needs_restore = false;
  root->needs_later_processing = false;

  root->damage = tickit_rectset_new();
  if(!root->damage) {
    tickit_window_destroy(ROOT_AS_WINDOW(root));
    return NULL;
  }

  root->event_id = tickit_term_bind_event(term, TICKIT_EV_RESIZE|TICKIT_EV_KEY|TICKIT_EV_MOUSE,
      0, &on_term, root);

  root->mouse_dragging = false;

  tickit_window_expose(ROOT_AS_WINDOW(root), NULL);

  return ROOT_AS_WINDOW(root);
}

static TickitRootWindow *_get_root(const TickitWindow *win)
{
  const TickitWindow *root = win;
  while(root->parent) {
    root = root->parent;
  }
  return WINDOW_AS_ROOT(root);
}

TickitWindow *tickit_window_new(TickitWindow *parent, TickitRect rect, TickitWindowFlags flags)
{
  if(flags & TICKIT_WINDOW_ROOT_PARENT)
    while(parent->parent) {
      rect.top  += parent->rect.top;
      rect.left += parent->rect.left;
      parent = parent->parent;
    }

  TickitWindow *win = malloc(sizeof(TickitWindow));
  if(!win)
    return NULL;

  init_window(win, parent, rect);

  if(flags & TICKIT_WINDOW_HIDDEN)
    win->is_visible = false;
  if(flags & TICKIT_WINDOW_STEAL_INPUT)
    win->steal_input = true;

  _do_hierarchy_change(
    (flags & TICKIT_WINDOW_LOWEST) ? TICKIT_HIERARCHY_INSERT_LAST : TICKIT_HIERARCHY_INSERT_FIRST,
    parent, win
  );

  return win;
}

TickitWindow *tickit_window_parent(const TickitWindow *win)
{
  return win->parent;
}

TickitWindow *tickit_window_root(const TickitWindow *win)
{
  return ROOT_AS_WINDOW(_get_root(win));
}

void tickit_window_destroy(TickitWindow *win)
{
  tickit_hooklist_unbind_and_destroy(win->hooks, win);

  if(win->pen)
    tickit_pen_unref(win->pen);

  for(TickitWindow *child = win->first_child; child; /**/) {
    TickitWindow *next = child->next;

    tickit_window_destroy(child);
    child = next;
  }

  _purge_hierarchy_changes(win);

  if(win->parent)
    _do_hierarchy_change(TICKIT_HIERARCHY_REMOVE, win->parent, win);

  /* Root cleanup */
  if(!win->parent) {
    TickitRootWindow *root = WINDOW_AS_ROOT(win);
    if(root->damage) {
      tickit_rectset_destroy(root->damage);
    }

    tickit_term_unbind_event_id(root->term, root->event_id);
  }

  free(win);
}

void tickit_window_raise(TickitWindow *win)
{
  _request_hierarchy_change(TICKIT_HIERARCHY_RAISE, win);
}

void tickit_window_raise_to_front(TickitWindow *win)
{
  _request_hierarchy_change(TICKIT_HIERARCHY_RAISE_FRONT, win);
}

void tickit_window_lower(TickitWindow *win)
{
  _request_hierarchy_change(TICKIT_HIERARCHY_LOWER, win);
}

void tickit_window_lower_to_back(TickitWindow *win)
{
  _request_hierarchy_change(TICKIT_HIERARCHY_LOWER_BACK, win);
}

void tickit_window_show(TickitWindow *win)
{
  win->is_visible = true;
  if(win->parent) {
    if(!win->parent->focused_child &&
       (win->focused_child || win->is_focused)) {
      win->parent->focused_child = win;
    }
  }
  tickit_window_expose(win, NULL);
}

void tickit_window_hide(TickitWindow *win)
{
  win->is_visible = false;

  if(win->parent) {
    TickitWindow *parent = win->parent;
    if(parent->focused_child && (parent->focused_child == win)) {
      parent->focused_child = NULL;
    }
    tickit_window_expose(parent, &win->rect);
  }
}

bool tickit_window_is_visible(TickitWindow *win)
{
  return win->is_visible;
}

TickitRect tickit_window_get_geometry(const TickitWindow *win)
{
  return win->rect;
}

TickitRect tickit_window_get_abs_geometry(const TickitWindow *win)
{
  TickitRect geom = win->rect;

  for(win = win->parent; win; win = win->parent)
    tickit_rect_translate(&geom, win->rect.top, win->rect.left);

  return geom;
}

int tickit_window_bottom(const TickitWindow *win)
{
  return win->rect.top + win->rect.lines;
}

int tickit_window_right(const TickitWindow *win)
{
  return win->rect.left + win->rect.cols;
}

void tickit_window_resize(TickitWindow *win, int lines, int cols)
{
  tickit_window_set_geometry(win, (TickitRect){
      .top   = win->rect.top,
      .left  = win->rect.left,
      .lines = lines,
      .cols  = cols}
  );
}

void tickit_window_reposition(TickitWindow *win, int top, int left)
{
  tickit_window_set_geometry(win, (TickitRect){
      .top   = top,
      .left  = left,
      .lines = win->rect.lines,
      .cols  = win->rect.cols}
  );

  if(win->is_focused)
    _request_restore(_get_root(win));
}

void tickit_window_set_geometry(TickitWindow *win, TickitRect geom)
{
  if((win->rect.top != geom.top) ||
     (win->rect.left != geom.left) ||
     (win->rect.lines != geom.lines) ||
     (win->rect.cols != geom.cols))
  {
    TickitGeomchangeEventInfo info = {
      .rect = geom,
      .oldrect = win->rect,
    };

    win->rect = geom;

    run_events(win, TICKIT_EV_GEOMCHANGE, &info);
  }
}

TickitPen *tickit_window_get_pen(const TickitWindow *win)
{
  return win->pen;
}

void tickit_window_set_pen(TickitWindow *win, TickitPen *pen)
{
  if(win->pen)
    tickit_pen_unref(win->pen);

  if(pen)
    win->pen = tickit_pen_ref(pen);
  else
    win->pen = NULL;
}

void tickit_window_expose(TickitWindow *win, const TickitRect *exposed)
{
  TickitRect selfrect = { .top = 0, .left = 0, .lines = win->rect.lines, .cols = win->rect.cols };
  TickitRect damaged;

  if(exposed) {
    if(!tickit_rect_intersect(&damaged, &selfrect, exposed))
      return;
  }
  else
    damaged = selfrect;

  if(!win->is_visible)
    return;

  if(win->parent) {
    tickit_rect_translate(&damaged, win->rect.top, win->rect.left);
    tickit_window_expose(win->parent, &damaged);
    return;
  }

  DEBUG_LOGF("Wd", "Damage root " RECT_PRINTF_FMT,
      RECT_PRINTF_ARGS(damaged));

  /* If we're here, then we're a root win. */
  TickitRootWindow *root = WINDOW_AS_ROOT(win);
  if(tickit_rectset_contains(root->damage, &damaged))
    return;

  tickit_rectset_add(root->damage, &damaged);

  root->needs_expose = true;
  _request_later_processing(root);
}

static char *_gen_indent(TickitWindow *win)
{
  int depth = 0;
  while(win->parent) {
    depth++;
    win = win->parent;
  }

  static size_t buflen = 0;
  static char *buf = NULL;

  if(depth * 2 >= buflen) {
    free(buf);
    buf = malloc(buflen = (depth * 2 + 1));
    buf[0] = 0;
  }

  for(char *s = buf; depth; depth--, s += 2)
    s[0] = '|', s[1] = ' ';

  return buf;
}

static void _do_expose(TickitWindow *win, const TickitRect *rect, TickitRenderBuffer *rb)
{
  DEBUG_LOGF("Wx", "%sExpose " WINDOW_PRINTF_FMT " " RECT_PRINTF_FMT,
      _gen_indent(win), WINDOW_PRINTF_ARGS(win), RECT_PRINTF_ARGS(*rect));

  if(win->pen)
    tickit_renderbuffer_setpen(rb, win->pen);

  for(TickitWindow* child = win->first_child; child; child = child->next) {
    if(!child->is_visible)
      continue;

    TickitRect exposed;
    if(tickit_rect_intersect(&exposed, rect, &child->rect)) {
      tickit_renderbuffer_save(rb);

      tickit_renderbuffer_clip(rb, &exposed);
      tickit_renderbuffer_translate(rb, child->rect.top, child->rect.left);
      tickit_rect_translate(&exposed, -child->rect.top, -child->rect.left);
      _do_expose(child, &exposed, rb);

      tickit_renderbuffer_restore(rb);
    }

    tickit_renderbuffer_mask(rb, &child->rect);
  }

  TickitExposeEventInfo info = {
    .rect = *rect,
    .rb = rb,
  };
  run_events(win, TICKIT_EV_EXPOSE, &info);
}

static void _request_restore(TickitRootWindow *root)
{
  root->needs_restore = true;
  _request_later_processing(root);
}

static void _request_later_processing(TickitRootWindow *root)
{
  root->needs_later_processing = true;
}

static bool _cell_visible(TickitWindow *win, int line, int col)
{
  TickitWindow *prev = NULL;
  while(win) {
    if(line < 0 || line >= win->rect.lines ||
       col  < 0 || col  >= win->rect.cols)
      return false;

    for(TickitWindow *child = win->first_child; child; child = child->next) {
      if(prev && child == prev)
        break;
      if(!child->is_visible)
        continue;

      if(line < child->rect.top  || line >= child->rect.top + child->rect.lines)
        continue;
      if(col  < child->rect.left || col  >= child->rect.left + child->rect.cols)
        continue;

      return false;
    }

    line += win->rect.top;
    col  += win->rect.left;

    prev = win;
    win = win->parent;
  }

  return true;
}

static void _do_restore(TickitRootWindow *root)
{
  TickitWindow *win = ROOT_AS_WINDOW(root);

  while(win) {
    if(!win->is_visible)
      break;

    if(!win->focused_child)
      break;

    win = win->focused_child;
  }

  if(win && win->is_focused && win->cursor.visible &&
     _cell_visible(win, win->cursor.line, win->cursor.col)) {
    tickit_term_setctl_int(root->term, TICKIT_TERMCTL_CURSORVIS, 1);
    TickitRect abs_geom = tickit_window_get_abs_geometry(win);
    int cursor_line = win->cursor.line + abs_geom.top;
    int cursor_col = win->cursor.col + abs_geom.left;
    tickit_term_goto(root->term, cursor_line, cursor_col);
    tickit_term_setctl_int(root->term, TICKIT_TERMCTL_CURSORSHAPE, win->cursor.shape);
  }
  else
    tickit_term_setctl_int(root->term, TICKIT_TERMCTL_CURSORVIS, 0);

  tickit_term_flush(root->term);
}

void tickit_window_flush(TickitWindow *win)
{
  if(win->parent)
    // Can't flush non-root.
    return;

  TickitRootWindow *root = WINDOW_AS_ROOT(win);
  if(!root->needs_later_processing)
    return;

  root->needs_later_processing = false;

  if(root->hierarchy_changes) {
    HierarchyChange *req = root->hierarchy_changes;
    while(req) {
      _do_hierarchy_change(req->change, req->parent, req->win);
      HierarchyChange *next = req->next;
      free(req);
      req = next;
    }
    root->hierarchy_changes = NULL;
  }

  if(root->needs_expose) {
    root->needs_expose = false;

    TickitWindow *root_window = ROOT_AS_WINDOW(root);
    TickitRenderBuffer *rb = tickit_renderbuffer_new(root_window->rect.lines, root_window->rect.cols);

    int damage_count = tickit_rectset_rects(root->damage);
    TickitRect *rects = malloc(damage_count * sizeof(TickitRect));
    tickit_rectset_get_rects(root->damage, rects, damage_count);

    tickit_rectset_clear(root->damage);

    for(int i = 0; i < damage_count; i++) {
      TickitRect *rect = &rects[i];
      tickit_renderbuffer_save(rb);
      tickit_renderbuffer_clip(rb, rect);
      _do_expose(root_window, rect, rb);
      tickit_renderbuffer_restore(rb);
    }

    free(rects);

    tickit_renderbuffer_flush_to_term(rb, root->term);
    tickit_renderbuffer_destroy(rb);

    root->needs_restore = true;
  }

  if(root->needs_restore) {
    root->needs_restore = false;
    _do_restore(root);
  }
}

static TickitWindow **_find_child(TickitWindow *parent, TickitWindow *win)
{
  TickitWindow **winp = &parent->first_child;
  while(*winp && *winp != win)
    winp = &(*winp)->next;

  return winp;
}

static void _do_hierarchy_insert_first(TickitWindow *parent, TickitWindow *win)
{
  win->next = parent->first_child;
  parent->first_child = win;
}

static void _do_hierarchy_insert_last(TickitWindow *parent, TickitWindow *win)
{
  TickitWindow **lastp = &parent->first_child;
  while(*lastp)
    lastp = &(*lastp)->next;

  *lastp = win;
  win->next = NULL;
}

static void _do_hierarchy_remove(TickitWindow *parent, TickitWindow *win)
{
  TickitWindow **winp = _find_child(parent, win);
  if(!winp)
    return;

  *winp = (*winp)->next;
  win->next = NULL;
}

static void _do_hierarchy_raise(TickitWindow *parent, TickitWindow *win)
{
  TickitWindow **prevp = &parent->first_child;
  if(*prevp == win) // already first
    return;

  while(*prevp && (*prevp)->next != win)
    prevp = &(*prevp)->next;

  if(!prevp) // not found
    return;

  TickitWindow *after  = win->next;

  win->next = *prevp;
  (*prevp)->next = after;
  *prevp = win;
}

static void _do_hierarchy_lower(TickitWindow *parent, TickitWindow *win)
{
  TickitWindow **winp = _find_child(parent, win);
  if(!winp) // not found
    return;

  TickitWindow *after = win->next;
  if(!after) // already last
    return;

  win->next = after->next;
  *winp = after;
  after->next = win;
}

static void _do_hierarchy_change(HierarchyChangeType change, TickitWindow *parent, TickitWindow *win)
{
  const char *fmt = NULL;

  switch(change) {
    case TICKIT_HIERARCHY_INSERT_FIRST:
       fmt = "Window " WINDOW_PRINTF_FMT " adds " WINDOW_PRINTF_FMT;
      _do_hierarchy_insert_first(parent, win);
      break;
    case TICKIT_HIERARCHY_INSERT_LAST:
      fmt = "Window " WINDOW_PRINTF_FMT " adds " WINDOW_PRINTF_FMT;
      _do_hierarchy_insert_last(parent, win);
      break;
    case TICKIT_HIERARCHY_REMOVE:
      fmt = "Window " WINDOW_PRINTF_FMT " removes " WINDOW_PRINTF_FMT;
      _do_hierarchy_remove(parent, win);
      if(parent->focused_child && parent->focused_child == win)
        parent->focused_child = NULL;
      break;
    case TICKIT_HIERARCHY_RAISE:
      fmt = "Window " WINDOW_PRINTF_FMT " raises " WINDOW_PRINTF_FMT;
      _do_hierarchy_raise(parent, win);
      break;
    case TICKIT_HIERARCHY_RAISE_FRONT:
      fmt = "Window " WINDOW_PRINTF_FMT " raises " WINDOW_PRINTF_FMT " to front";
      _do_hierarchy_remove(parent, win);
      _do_hierarchy_insert_first(parent, win);
      break;
    case TICKIT_HIERARCHY_LOWER:
      fmt = "Window " WINDOW_PRINTF_FMT " lowers " WINDOW_PRINTF_FMT;
      _do_hierarchy_lower(parent, win);
      break;
    case TICKIT_HIERARCHY_LOWER_BACK:
      fmt = "Window " WINDOW_PRINTF_FMT " lowers " WINDOW_PRINTF_FMT " to back";
      _do_hierarchy_remove(parent, win);
      _do_hierarchy_insert_last(parent, win);
      break;
  }

  if(fmt)
    DEBUG_LOGF("Wh", fmt,
        WINDOW_PRINTF_ARGS(parent), WINDOW_PRINTF_ARGS(win));

  tickit_window_expose(parent, &win->rect);
}

static void _request_hierarchy_change(HierarchyChangeType change, TickitWindow *win)
{
  if(!win->parent)
    /* Can't do anything to the root win */
    return;

  HierarchyChange *req = malloc(sizeof(HierarchyChange));
  req->change = change;
  req->parent = win->parent;
  req->win = win;
  req->next = NULL;
  TickitRootWindow *root = _get_root(win);
  if(!root->hierarchy_changes) {
    root->hierarchy_changes = req;
    _request_later_processing(root);
  }
  else {
    HierarchyChange *chain = root->hierarchy_changes;
    while(chain->next) {
      chain = chain->next;
    }
    chain->next = req;
  }
}

static void _purge_hierarchy_changes(TickitWindow *win)
{
  TickitRootWindow *root = _get_root(win);
  HierarchyChange **changep = &root->hierarchy_changes;
  while(*changep) {
    HierarchyChange *req = *changep;
    if(req->parent == win || req->win == win) {
      *changep = req->next;
      free(req);
    }
    else
      changep = &req->next;
  }
}

static bool _scrollrectset(TickitWindow *win, TickitRectSet *visible, int downward, int rightward, TickitPen *pen)
{
  TickitWindow *origwin = win;

  int abs_top = 0, abs_left = 0;

  while(win) {
    if(!win->is_visible)
      return false;

    tickit_pen_copy(pen, win->pen, false);

    TickitWindow *parent = win->parent;
    if(!parent)
      break;

    abs_top  += win->rect.top;
    abs_left += win->rect.left;
    tickit_rectset_translate(visible, win->rect.top, win->rect.left);

    for(TickitWindow *sib = parent->first_child; sib; sib = sib->next) {
      if(sib == win)
        break;
      if(!sib->is_visible)
        continue;

      tickit_rectset_subtract(visible, &sib->rect);
    }

    win = parent;
  }

  TickitTerm *term = WINDOW_AS_ROOT(win)->term;

  int n = tickit_rectset_rects(visible);
  TickitRect rects[n];
  tickit_rectset_get_rects(visible, rects, n);

  bool ret = true;
  bool done_pen = false;

  for(int i = 0; i < n; i++) {
    TickitRect rect = rects[i];

    TickitRect origrect = rect;
    tickit_rect_translate(&origrect, -abs_top, -abs_left);

    if(abs(downward) >= rect.lines || abs(rightward) >= rect.cols) {
      tickit_window_expose(origwin, &origrect);
      continue;
    }

    // TODO: This may be more efficiently done with some rectset operations
    //   instead of completely resetting and rebuilding the set
    TickitRectSet *damageset = WINDOW_AS_ROOT(win)->damage;
    int n_damage = tickit_rectset_rects(damageset);
    TickitRect damage[n_damage];
    tickit_rectset_get_rects(damageset, damage, n_damage);
    tickit_rectset_clear(damageset);

    for(int j = 0; j < n_damage; j++) {
      TickitRect r = damage[j];

      if(tickit_rect_bottom(&r) < rect.top || r.top > tickit_rect_bottom(&rect) ||
         tickit_rect_right(&r) < rect.left || r.left > tickit_rect_right(&rect)) {
        tickit_rectset_add(damageset, &r);
        continue;
      }

      TickitRect outside[4];
      int n_outside;
      if((n_outside = tickit_rect_subtract(outside, &r, &rect))) {
        for(int k = 0; k < n_outside; k++)
          tickit_rectset_add(damageset, outside+k);
      }

      TickitRect inside;
      if(tickit_rect_intersect(&inside, &r, &rect)) {
        tickit_rect_translate(&inside, -downward, -rightward);
        if(tickit_rect_intersect(&inside, &inside, &rect))
          tickit_rectset_add(damageset, &inside);
      }
    }

    DEBUG_LOGF("Wsr", "Term scrollrect " RECT_PRINTF_FMT " by %+d,%+d",
      RECT_PRINTF_ARGS(rect), rightward, downward);

    if(!done_pen) {
      // TODO: only bg matters
      tickit_term_setpen(term, pen);
      done_pen = true;
    }

    if(tickit_term_scrollrect(term, rect, downward, rightward)) {
      if(downward > 0) {
        // "scroll down" means lines moved upward, so the bottom needs redrawing
        tickit_window_expose(origwin, &(TickitRect){
            .top  = origrect.top + origrect.lines - downward, .lines = downward,
            .left = origrect.left,                            .cols  = rect.cols
        });
      }
      else if(downward < 0) {
        // "scroll up" means lines moved downward, so top needs redrawing
        tickit_window_expose(origwin, &(TickitRect){
            .top  = origrect.top,  .lines = -downward,
            .left = origrect.left, .cols  = rect.cols,
        });
      }

      if(rightward > 0) {
        // "scroll right" means columns moved leftward, so the right edge needs redrawing
        tickit_window_expose(origwin, &(TickitRect){
            .top  = origrect.top,                              .lines = rect.lines,
            .left = origrect.left + origrect.cols - rightward, .cols  = rightward,
        });
      }
      else if(rightward < 0) {
        tickit_window_expose(origwin, &(TickitRect){
            .top  = origrect.top,  .lines = rect.lines,
            .left = origrect.left, .cols  = -rightward,
        });
      }
    }
    else {
      tickit_window_expose(origwin, &origrect);
      ret = false;
    }
  }

  return ret;
}

static bool _scroll(TickitWindow *win, const TickitRect *origrect, int downward, int rightward, TickitPen *pen, bool mask_children)
{
  TickitRect rect;
  TickitRect selfrect = { .top = 0, .left = 0, .lines = win->rect.lines, .cols = win->rect.cols };

  if(!tickit_rect_intersect(&rect, &selfrect, origrect))
    return false;

  DEBUG_LOGF("Ws", "Scroll " RECT_PRINTF_FMT " by %+d,%+d",
    RECT_PRINTF_ARGS(rect), rightward, downward);

  if(pen)
    pen = tickit_pen_ref(pen);
  else
    pen = tickit_pen_new();

  TickitRectSet *visible = tickit_rectset_new();

  tickit_rectset_add(visible, &rect);

  if(mask_children)
    for(TickitWindow *child = win->first_child; child; child = child->next) {
      if(!child->is_visible)
        continue;
      tickit_rectset_subtract(visible, &child->rect);
    }

  bool ret = _scrollrectset(win, visible, downward, rightward, pen);

  tickit_rectset_destroy(visible);
  tickit_pen_unref(pen);

  return ret;
}

bool tickit_window_scrollrect(TickitWindow *win, const TickitRect *rect, int downward, int rightward, TickitPen *pen)
{
  return _scroll(win, rect, downward, rightward, pen, true);
}

bool tickit_window_scroll(TickitWindow *win, int downward, int rightward)
{
  return _scroll(win,
    &((TickitRect){ .top = 0, .left = 0, .lines = win->rect.lines, .cols = win->rect.cols }),
    downward, rightward, NULL, true);
}

bool tickit_window_scroll_with_children(TickitWindow *win, int downward, int rightward)
{
  return _scroll(win,
    &((TickitRect){ .top = 0, .left = 0, .lines = win->rect.lines, .cols = win->rect.cols }),
    downward, rightward, NULL, false);
}

void tickit_window_set_cursor_position(TickitWindow *win, int line, int col)
{
  win->cursor.line = line;
  win->cursor.col = col;

  if(win->is_focused)
    _request_restore(_get_root(win));
}

void tickit_window_set_cursor_visible(TickitWindow *win, bool visible)
{
  win->cursor.visible = visible;

  if(win->is_focused)
    _request_restore(_get_root(win));
}

void tickit_window_set_cursor_shape(TickitWindow *win, TickitCursorShape shape)
{
  win->cursor.shape = shape;

  if(win->is_focused)
    _request_restore(_get_root(win));
}

static void _focus_gained(TickitWindow *win, TickitWindow *child);
static void _focus_lost(TickitWindow *win);

void tickit_window_take_focus(TickitWindow *win)
{
  _focus_gained(win, NULL);
}

static void _focus_gained(TickitWindow *win, TickitWindow *child)
{
  if(win->focused_child && child && win->focused_child != child)
    _focus_lost(win->focused_child);

  if(win->parent) {
    if(win->is_visible)
      _focus_gained(win->parent, win);
  }
  else
    _request_restore(_get_root(win));

  if(!child) {
    win->is_focused = true;

    TickitFocusEventInfo info = { .type = TICKIT_FOCUSEV_IN, .win = win };
    run_events(win, TICKIT_EV_FOCUS, &info);
  }
  else if(win->focus_child_notify) {
    TickitFocusEventInfo info = { .type = TICKIT_FOCUSEV_IN, .win = child };
    run_events(win, TICKIT_EV_FOCUS, &info);
  }

  win->focused_child = child;
}

static void _focus_lost(TickitWindow *win)
{
  if(win->focused_child) {
    _focus_lost(win->focused_child);

    if(win->focus_child_notify) {
      TickitFocusEventInfo info = { .type = TICKIT_FOCUSEV_OUT, .win = win->focused_child };
      run_events(win, TICKIT_EV_FOCUS, &info);
    }
  }

  if(win->is_focused) {
    win->is_focused = false;
    TickitFocusEventInfo info = { .type = TICKIT_FOCUSEV_OUT, .win = win };
    run_events(win, TICKIT_EV_FOCUS, &info);
  }
}

bool tickit_window_is_focused(const TickitWindow *win)
{
  return win->is_focused;
}

void tickit_window_set_focus_child_notify(TickitWindow *win, bool notify)
{
  win->focus_child_notify = notify;
}

static int _handle_key(TickitWindow *win, TickitKeyEventInfo *info)
{
  if(!win->is_visible)
    return 0;

  if(win->first_child && win->first_child->steal_input)
    if(_handle_key(win->first_child, info))
      return 1;

  if(win->focused_child)
    if(_handle_key(win->focused_child, info))
      return 1;

  if(run_events_whilefalse(win, TICKIT_EV_KEY, info))
    return 1;

  // Last-ditch attempt to spread it around other children
  for(TickitWindow *child = win->first_child; child; child = child->next) {
    if(child == win->focused_child)
      continue;

    if(_handle_key(child, info))
      return 1;
  }

  return 0;
}

static TickitWindow *_handle_mouse(TickitWindow *win, TickitMouseEventInfo *info)
{
  if(!win->is_visible)
    return NULL;

  for(TickitWindow *child = win->first_child; child; child = child->next) {
    int child_line = info->line - child->rect.top;
    int child_col  = info->col  - child->rect.left;

    if(!child->steal_input) {
      if(child_line < 0 || child_line >= child->rect.lines)
        continue;
      if(child_col < 0 || child_col >= child->rect.cols)
        continue;
    }

    TickitMouseEventInfo childinfo = *info;
    childinfo.line = child_line;
    childinfo.col  = child_col;

    TickitWindow *ret;
    if((ret = _handle_mouse(child, &childinfo)))
      return ret;
  }

  if(run_events_whilefalse(win, TICKIT_EV_MOUSE, info))
    return win;

  return 0;
}
