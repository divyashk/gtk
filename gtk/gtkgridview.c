/*
 * Copyright © 2019 Benjamin Otte
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library. If not, see <http://www.gnu.org/licenses/>.
 *
 * Authors: Benjamin Otte <otte@gnome.org>
 */

#include "config.h"

#include "gtkgridview.h"

#include "gtkadjustment.h"
#include "gtkintl.h"
#include "gtklistitemfactory.h"
#include "gtklistitemmanagerprivate.h"
#include "gtkorientableprivate.h"
#include "gtkprivate.h"
#include "gtkscrollable.h"
#include "gtksingleselection.h"
#include "gtktypebuiltins.h"
#include "gtkwidgetprivate.h"

/* Maximum number of list items created by the gridview.
 * For debugging, you can set this to G_MAXUINT to ensure
 * there's always a list item for every row.
 *
 * We multiply this number with GtkGridView:max-columns so
 * that we can always display at least this many rows.
 */
#define GTK_GRID_VIEW_MIN_VISIBLE_ROWS (30)

#define DEFAULT_MAX_COLUMNS (7)

/**
 * SECTION:gtkgridview
 * @title: GtkGridView
 * @short_description: A widget for displaying lists in a grid
 * @see_also: #GListModel
 *
 * GtkGridView is a widget to present a view into a large dynamic grid of items.
 */

typedef struct _Cell Cell;
typedef struct _CellAugment CellAugment;

struct _GtkGridView
{
  GtkWidget parent_instance;

  GListModel *model;
  GtkListItemManager *item_manager;
  GtkAdjustment *adjustment[2];
  GtkScrollablePolicy scroll_policy[2];
  GtkOrientation orientation;
  guint min_columns;
  guint max_columns;
  /* set in size_allocate */
  guint n_columns;
  double column_width;
  int unknown_row_height;

  GtkListItemTracker *anchor;
  double anchor_xalign;
  double anchor_yalign;
  guint anchor_xstart : 1;
  guint anchor_ystart : 1;
};

struct _Cell
{
  GtkListItemManagerItem parent;
  guint size; /* total, only counting cells in first column */
};

struct _CellAugment
{
  GtkListItemManagerItemAugment parent;
  guint size; /* total, only counting first column */
};

enum
{
  PROP_0,
  PROP_FACTORY,
  PROP_HADJUSTMENT,
  PROP_HSCROLL_POLICY,
  PROP_MAX_COLUMNS,
  PROP_MIN_COLUMNS,
  PROP_MODEL,
  PROP_ORIENTATION,
  PROP_VADJUSTMENT,
  PROP_VSCROLL_POLICY,

  N_PROPS
};

G_DEFINE_TYPE_WITH_CODE (GtkGridView, gtk_grid_view, GTK_TYPE_WIDGET,
                         G_IMPLEMENT_INTERFACE (GTK_TYPE_ORIENTABLE, NULL)
                         G_IMPLEMENT_INTERFACE (GTK_TYPE_SCROLLABLE, NULL))

static GParamSpec *properties[N_PROPS] = { NULL, };

static void G_GNUC_UNUSED
dump (GtkGridView *self)
{
  Cell *cell;
  guint n_widgets, n_list_rows, n_items;

  n_widgets = 0;
  n_list_rows = 0;
  n_items = 0;
  //g_print ("ANCHOR: %u - %u\n", self->anchor_start, self->anchor_end);
  for (cell = gtk_list_item_manager_get_first (self->item_manager);
       cell;
       cell = gtk_rb_tree_node_get_next (cell))
    {
      if (cell->parent.widget)
        n_widgets++;
      n_list_rows++;
      n_items += cell->parent.n_items;
      g_print ("%6u%6u %5ux%3u %s (%upx)\n",
               cell->parent.n_items, n_items,
               n_items / (self->n_columns ? self->n_columns : self->min_columns),
               n_items % (self->n_columns ? self->n_columns : self->min_columns),
               cell->parent.widget ? " (widget)" : "", cell->size);
    }

  g_print ("  => %u widgets in %u list rows\n", n_widgets, n_list_rows);
}

/*<private>
 * gtk_grid_view_get_cell_at_y:
 * @self: a #GtkGridView
 * @y: an offset in direction of @self's orientation
 * @position: (out caller-allocates) (optional): stores the position
 *     index of the returned row
 * @offset: (out caller-allocates) (optional): stores the offset
 *     in pixels between y and top of cell.
 * @offset: (out caller-allocates) (optional): stores the height
 *     of the cell
 *
 * Gets the Cell that occupies the leftmost position in the row at offset
 * @y into the primary direction. 
 *
 * If y is larger than the height of all cells, %NULL will be returned.
 * In particular that means that for an emtpy grid, %NULL is returned
 * for any value.
 *
 * Returns: (nullable): The first cell at offset y or %NULL if none
 **/
static Cell *
gtk_grid_view_get_cell_at_y (GtkGridView *self,
                             int          y,
                             guint       *position,
                             int         *offset,
                             int         *size)
{
  Cell *cell, *tmp;
  guint pos;

  cell = gtk_list_item_manager_get_root (self->item_manager);
  pos = 0;

  while (cell)
    {
      tmp = gtk_rb_tree_node_get_left (cell);
      if (tmp)
        {
          CellAugment *aug = gtk_list_item_manager_get_item_augment (self->item_manager, tmp);
          if (y < aug->size)
            {
              cell = tmp;
              continue;
            }
          y -= aug->size;
          pos += aug->parent.n_items;
        }

      if (y < cell->size)
        break;
      y -= cell->size;
      pos += cell->parent.n_items;

      cell = gtk_rb_tree_node_get_right (cell);
    }

  if (cell == NULL)
    {
      if (position)
        *position = 0;
      if (offset)
        *offset = 0;
      if (size)
        *size = 0;
      return NULL;
    }

  /* We know have the (range of) cell(s) that contains this offset.
   * Now for the hard part of computing which index this actually is.
   */
  if (offset || position || size)
    {
      guint n_items = cell->parent.n_items;
      guint no_widget_rows, skip;

      /* skip remaining items at end of row */
      if (pos % self->n_columns)
        {
          skip = pos - pos % self->n_columns;
          n_items -= skip;
          pos += skip;
        }

      /* Skip all the rows this index doesn't go into */
      no_widget_rows = (n_items - 1) / self->n_columns;
      skip = MIN (y / self->unknown_row_height, no_widget_rows);
      y -= skip * self->unknown_row_height;
      pos += self->n_columns * skip;

      if (position)
        *position = pos;
      if (offset)
        *offset = y;
      if (size)
        {
          if (skip < no_widget_rows)
            *size = self->unknown_row_height;
          else
            *size = cell->size - no_widget_rows * self->unknown_row_height;
        }
    }

  return cell;
}

/*<private>
 * gtk_grid_view_get_size_at_position:
 * @self: a #GtkGridView
 * @position: position of the item
 * @offset: (out caller-allocates) (optional): stores the y coordinate 
 *     of the cell (x coordinate for horizontal grids)
 * @size: (out caller-allocates) (optional): stores the height
 *     of the cell (width for horizontal grids)
 *
 * Computes where the cell at @position is allocated.
 *
 * If position is larger than the number of items, %FALSE will be returned.
 * In particular that means that for an emtpy grid, %FALSE is returned
 * for any value.
 *
 * Returns: (nullable): %TRUE if the cell existed, %FALSE otherwise
 **/
static gboolean
gtk_grid_view_get_size_at_position (GtkGridView *self,
                                    guint        position,
                                    int         *offset,
                                    int         *size)
{
  Cell *cell, *tmp;
  int y;

  cell = gtk_list_item_manager_get_root (self->item_manager);
  y = 0;
  position -= position % self->n_columns;

  while (cell)
    {
      tmp = gtk_rb_tree_node_get_left (cell);
      if (tmp)
        {
          CellAugment *aug = gtk_list_item_manager_get_item_augment (self->item_manager, tmp);
          if (position < aug->parent.n_items)
            {
              cell = tmp;
              continue;
            }
          position -= aug->parent.n_items;
          y += aug->size;
        }

      if (position < cell->parent.n_items)
        break;
      y += cell->size;
      position -= cell->parent.n_items;

      cell = gtk_rb_tree_node_get_right (cell);
    }

  if (cell == NULL)
    {
      if (offset)
        *offset = 0;
      if (size)
        *size = 0;
      return FALSE;
    }

  /* We know have the (range of) cell(s) that contains this offset.
   * Now for the hard part of computing which index this actually is.
   */
  if (offset || size)
    {
      guint n_items = cell->parent.n_items;
      guint skip;

      /* skip remaining items at end of row */
      if (position % self->n_columns)
        {
          skip = position % self->n_columns;
          n_items -= skip;
          position -= skip;
        }

      /* Skip all the rows this index doesn't go into */
      skip = position / self->n_columns;
      n_items -= skip * self->n_columns;
      y += skip * self->unknown_row_height;

      if (offset)
        *offset = y;
      if (size)
        {
          if (n_items > self->n_columns)
            *size = self->unknown_row_height;
          else
            *size = cell->size - skip * self->unknown_row_height;
        }
    }

  return TRUE;
}

static void
gtk_grid_view_set_anchor (GtkGridView *self,
                          guint        position,
                          double       xalign,
                          gboolean     xstart,
                          double       yalign,
                          gboolean     ystart)
{
  gtk_list_item_tracker_set_position (self->item_manager,
                                      self->anchor,
                                      position,
                                      (GTK_GRID_VIEW_MIN_VISIBLE_ROWS * yalign + 1) * self->max_columns,
                                      (GTK_GRID_VIEW_MIN_VISIBLE_ROWS * (1 - yalign) + 1) * self->max_columns);

  if (self->anchor_xalign != xalign ||
      self->anchor_xstart != xstart ||
      self->anchor_yalign != yalign ||
      self->anchor_ystart != ystart)
    {
      self->anchor_xalign = xalign;
      self->anchor_xstart = xstart;
      self->anchor_yalign = yalign;
      self->anchor_ystart = ystart;
      gtk_widget_queue_allocate (GTK_WIDGET (self));
    }
}

static gboolean
gtk_grid_view_adjustment_is_flipped (GtkGridView    *self,
                                     GtkOrientation  orientation)
{
  if (orientation == GTK_ORIENTATION_VERTICAL)
    return FALSE;

  return gtk_widget_get_direction (GTK_WIDGET (self)) == GTK_TEXT_DIR_RTL;
}

static void
gtk_grid_view_adjustment_value_changed_cb (GtkAdjustment *adjustment,
                                           GtkGridView   *self)
{
  int page_size, total_size, value, from_start;
  guint pos, anchor_pos, n_items;
  int offset, height, top, bottom;
  double xalign, yalign;
  gboolean xstart, ystart;

  page_size = gtk_adjustment_get_page_size (adjustment);
  value = gtk_adjustment_get_value (adjustment);
  total_size = gtk_adjustment_get_upper (adjustment);
  anchor_pos = gtk_list_item_tracker_get_position (self->item_manager, self->anchor);
  n_items = g_list_model_get_n_items (self->model);
  if (gtk_grid_view_adjustment_is_flipped (self, self->orientation))
    value = total_size - page_size - value;

  if (adjustment == self->adjustment[self->orientation])
    {
      /* Compute how far down we've scrolled. That's the height
       * we want to align to. */
      yalign = (double) value / (total_size - page_size);
      from_start = round (yalign * page_size);
      
      /* We want the cell that far down the page */
      if (gtk_grid_view_get_cell_at_y (self,
                                       value + from_start,
                                       &pos,
                                       &offset,
                                       &height))
        {
          /* offset from value - which is where we wanna scroll to */
          top = from_start - offset;
          bottom = top + height;

          /* find an anchor that is in the visible area */
          if (top > 0 && bottom < page_size)
            ystart = from_start - top <= bottom - from_start;
          else if (top > 0)
            ystart = TRUE;
          else if (bottom < page_size)
            ystart = FALSE;
          else
            {
              /* This is the case where the cell occupies the whole visible area.
               * It's also the only case where align will not end up in [0..1] */
              ystart = from_start - top <= bottom - from_start;
            }

          /* Now compute the align so that when anchoring to the looked
           * up cell, the position is pixel-exact.
           */
          yalign = (double) (ystart ? top : bottom) / page_size;
        }
      else
        {
          /* Happens if we scroll down to the end - we will query
           * exactly the pixel behind the last one we can get a cell for.
           * So take the last row. */
          pos = n_items - 1;
          pos = pos - pos % self->n_columns;
          yalign = 1.0;
          ystart = FALSE;
        }

      /* And finally, keep the column anchor intact. */
      anchor_pos %= self->n_columns;
      pos += anchor_pos;
      xstart = self->anchor_xstart;
      xalign = self->anchor_xalign;
    }
  else
    {
      xalign = (double) value / (total_size - page_size);
      from_start = round (xalign * page_size);
      pos = floor ((value + from_start) / self->column_width);
      if (pos >= self->n_columns)
        {
          /* scrolling to the end sets pos to exactly self->n_columns */
          pos = self->n_columns - 1;
          xstart = FALSE;
          xalign = 1.0;
        }
      else
        {
          top = ceil (self->column_width * pos) - value;
          bottom = ceil (self->column_width * (pos + 1)) - value;
          
          /* offset from value - which is where we wanna scroll to */

          /* find an anchor that is in the visible area */
          if (top > 0 && bottom < page_size)
            xstart = from_start - top <= bottom - from_start;
          else if (top > 0)
            xstart = TRUE;
          else if (bottom < page_size)
            xstart = FALSE;
          else
            xstart = from_start - top <= bottom - from_start;

          xalign = (double) (xstart ? top : bottom) / page_size;
        }

      /* And finally, keep the row anchor intact. */
      pos += (anchor_pos - anchor_pos % self->n_columns);
      yalign = self->anchor_yalign;
      ystart = self->anchor_ystart;
    }

  if (pos >= n_items)
    {
      /* Ugh, we're in the last row and don't have enough items
       * to fill the row.
       * Do it the hard way then... */
      adjustment = self->adjustment[OPPOSITE_ORIENTATION (self->orientation)];

      pos = n_items - 1;
      xstart = FALSE;
      xalign = (ceil (self->column_width * (pos % self->n_columns + 1)) 
                - gtk_adjustment_get_value (adjustment))
               / gtk_adjustment_get_page_size (adjustment);
    }

  gtk_grid_view_set_anchor (self, pos, xalign, xstart, yalign, ystart);
  
  gtk_widget_queue_allocate (GTK_WIDGET (self));
}

static int
gtk_grid_view_update_adjustment_with_values (GtkGridView    *self,
                                             GtkOrientation  orientation,
                                             int             value,
                                             int             upper,
                                             int             page_size)
{
  upper = MAX (upper, page_size);
  value = MAX (0, value);
  value = MIN (value, upper - page_size);

  g_signal_handlers_block_by_func (self->adjustment[orientation],
                                   gtk_grid_view_adjustment_value_changed_cb,
                                   self);
  gtk_adjustment_configure (self->adjustment[orientation],
                            gtk_grid_view_adjustment_is_flipped (self, orientation)
                                ? value = upper - page_size - value
                                : value,
                            0,
                            upper,
                            page_size * 0.1,
                            page_size * 0.9,
                            page_size);
  g_signal_handlers_unblock_by_func (self->adjustment[orientation],
                                     gtk_grid_view_adjustment_value_changed_cb,
                                     self);

  return value;
}

static int
gtk_grid_view_update_adjustment (GtkGridView    *self,
                                 GtkOrientation  orientation)
{
  int value, page_size, cell_size, total_size;
  guint anchor_pos;

  anchor_pos = gtk_list_item_tracker_get_position (self->item_manager, self->anchor);
  if (anchor_pos == GTK_INVALID_LIST_POSITION)
    {
      gtk_grid_view_update_adjustment_with_values (self, orientation, 0, 0, 0);
      return 0;
    }

  page_size = gtk_widget_get_size (GTK_WIDGET (self), orientation);

  if (self->orientation == orientation)
    {
      Cell *cell;
      CellAugment *aug;

      cell = gtk_list_item_manager_get_root (self->item_manager);
      g_assert (cell);
      aug = gtk_list_item_manager_get_item_augment (self->item_manager, cell);
      if (!gtk_grid_view_get_size_at_position (self,
                                               anchor_pos,
                                               &value,
                                               &cell_size))
        {
          g_assert_not_reached ();
        }
      if (!self->anchor_ystart)
        value += cell_size;

      value = gtk_grid_view_update_adjustment_with_values (self,
                                                           self->orientation,
                                                           value - self->anchor_yalign * page_size,
                                                           aug->size,
                                                           page_size);
    }
  else
    {
      guint i = anchor_pos % self->n_columns;

      if (self->anchor_xstart)
        value = ceil (self->column_width * i);
      else
        value = ceil (self->column_width * (i + 1));
      total_size = round (self->n_columns * self->column_width);

      value = gtk_grid_view_update_adjustment_with_values (self,
                                                           OPPOSITE_ORIENTATION (self->orientation),
                                                           value - self->anchor_xalign * page_size,
                                                           total_size,
                                                           page_size);
    }

  return value;
}

static int
compare_ints (gconstpointer first,
              gconstpointer second)
{
  return *(int *) first - *(int *) second;
}

static int
gtk_grid_view_get_unknown_row_size (GtkGridView *self,
                                    GArray      *heights)
{
  g_return_val_if_fail (heights->len > 0, 0);

  /* return the median and hope rows are generally uniform with few outliers */
  g_array_sort (heights, compare_ints);

  return g_array_index (heights, int, heights->len / 2);
}

static void
gtk_grid_view_measure_column_size (GtkGridView *self,
                                   int         *minimum,
                                   int         *natural)
{
  GtkOrientation opposite;
  Cell *cell;
  int min, nat, child_min, child_nat;

  min = 0;
  nat = 0;
  opposite = OPPOSITE_ORIENTATION (self->orientation);

  for (cell = gtk_list_item_manager_get_first (self->item_manager);
       cell != NULL;
       cell = gtk_rb_tree_node_get_next (cell))
    {
      /* ignore unavailable cells */
      if (cell->parent.widget == NULL)
        continue;

      gtk_widget_measure (cell->parent.widget,
                          opposite, -1,
                          &child_min, &child_nat, NULL, NULL);
      min = MAX (min, child_min);
      nat = MAX (nat, child_nat);
    }

  *minimum = min;
  *natural = nat;
}

static void
gtk_grid_view_measure_across (GtkWidget *widget,
                              int        for_size,
                              int       *minimum,
                              int       *natural)
{
  GtkGridView *self = GTK_GRID_VIEW (widget);

  gtk_grid_view_measure_column_size (self, minimum, natural);

  *minimum *= self->min_columns;
  *natural *= self->max_columns;
}

static guint
gtk_grid_view_compute_n_columns (GtkGridView *self,
                                 guint        for_size,
                                 int          min,
                                 int          nat)
{
  guint n_columns;

  /* rounding down is exactly what we want here, so int division works */
  if (self->scroll_policy[OPPOSITE_ORIENTATION (self->orientation)] == GTK_SCROLL_MINIMUM)
    n_columns = for_size / MAX (1, min);
  else
    n_columns = for_size / MAX (1, nat);

  n_columns = CLAMP (n_columns, self->min_columns, self->max_columns);

  return n_columns;
}

static void
gtk_grid_view_measure_list (GtkWidget *widget,
                            int        for_size,
                            int       *minimum,
                            int       *natural)
{
  GtkGridView *self = GTK_GRID_VIEW (widget);
  Cell *cell;
  int height, row_height, child_min, child_nat, column_size, col_min, col_nat;
  gboolean measured;
  GArray *heights;
  guint n_unknown, n_columns;
  guint i;

  heights = g_array_new (FALSE, FALSE, sizeof (int));
  n_unknown = 0;
  height = 0;

  gtk_grid_view_measure_column_size (self, &col_min, &col_nat);
  for_size = MAX (for_size, col_min * (int) self->min_columns);
  n_columns = gtk_grid_view_compute_n_columns (self, for_size, col_min, col_nat);
  column_size = for_size / n_columns;

  i = 0;
  row_height = 0;
  measured = FALSE;
  for (cell = gtk_list_item_manager_get_first (self->item_manager);
       cell != NULL;
       cell = gtk_rb_tree_node_get_next (cell))
    {
      if (cell->parent.widget)
        {
          gtk_widget_measure (cell->parent.widget,
                              self->orientation, column_size,
                              &child_min, &child_nat, NULL, NULL);
          if (self->scroll_policy[self->orientation] == GTK_SCROLL_MINIMUM)
            row_height = MAX (row_height, child_min);
          else
            row_height = MAX (row_height, child_nat);
          measured = TRUE;
        }
      
      i += cell->parent.n_items;

      if (i >= n_columns)
        {
          if (measured)
            {
              g_array_append_val (heights, row_height);
              i -= n_columns;
              height += row_height;
              measured = FALSE;
              row_height = 0;
            }
          n_unknown += i / n_columns;
          i %= n_columns;
        }
    }

  if (i > 0)
    {
      if (measured)
        {
          g_array_append_val (heights, row_height);
          height += row_height;
        }
      else
        n_unknown++;
    }

  if (n_unknown)
    height += n_unknown * gtk_grid_view_get_unknown_row_size (self, heights);

  g_array_free (heights, TRUE);

  *minimum = height;
  *natural = height;
}

static void
gtk_grid_view_measure (GtkWidget      *widget,
                       GtkOrientation  orientation,
                       int             for_size,
                       int            *minimum,
                       int            *natural,
                       int            *minimum_baseline,
                       int            *natural_baseline)
{
  GtkGridView *self = GTK_GRID_VIEW (widget);

  if (orientation == self->orientation)
    gtk_grid_view_measure_list (widget, for_size, minimum, natural);
  else
    gtk_grid_view_measure_across (widget, for_size, minimum, natural);
}

static void
cell_set_size (Cell  *cell,
               guint  size)
{
  if (cell->size == size)
    return;

  cell->size = size;
  gtk_rb_tree_node_mark_dirty (cell);
}

static void
gtk_grid_view_size_allocate_child (GtkGridView *self,
                                   GtkWidget   *child,
                                   int          x,
                                   int          y,
                                   int          width,
                                   int          height)
{
  GtkAllocation child_allocation;

  if (self->orientation == GTK_ORIENTATION_VERTICAL)
    {
      child_allocation.x = x;
      child_allocation.y = y;
      child_allocation.width = width;
      child_allocation.height = height;
    }
  else if (_gtk_widget_get_direction (GTK_WIDGET (self)) == GTK_TEXT_DIR_LTR)
    {
      child_allocation.x = y;
      child_allocation.y = x;
      child_allocation.width = height;
      child_allocation.height = width;
    }
  else
    {
      int mirror_point = gtk_widget_get_width (GTK_WIDGET (self));

      child_allocation.x = mirror_point - y - height; 
      child_allocation.y = x;
      child_allocation.width = height;
      child_allocation.height = width;
    }

  gtk_widget_size_allocate (child, &child_allocation, -1);
}

static void
gtk_grid_view_size_allocate (GtkWidget *widget,
                             int        width,
                             int        height,
                             int        baseline)
{
  GtkGridView *self = GTK_GRID_VIEW (widget);
  Cell *cell, *start;
  GArray *heights;
  int row_height, col_min, col_nat;
  GtkOrientation opposite_orientation;
  gboolean known;
  int x, y;
  guint i;

  opposite_orientation = OPPOSITE_ORIENTATION (self->orientation);

  /* step 0: exit early if list is empty */
  if (gtk_list_item_manager_get_root (self->item_manager) == NULL)
    return;

  /* step 1: determine width of the list */
  gtk_grid_view_measure_column_size (self, &col_min, &col_nat);
  self->n_columns = gtk_grid_view_compute_n_columns (self, 
                                                     self->orientation == GTK_ORIENTATION_VERTICAL ? width : height,
                                                     col_min, col_nat);
  self->column_width = (self->orientation == GTK_ORIENTATION_VERTICAL ? width : height) / self->n_columns;
  self->column_width = MAX (self->column_width, col_min);

  /* step 2: determine height of known rows */
  heights = g_array_new (FALSE, FALSE, sizeof (int));

  i = 0;
  row_height = 0;
  start = NULL;
  for (cell = gtk_list_item_manager_get_first (self->item_manager);
       cell != NULL;
       cell = gtk_rb_tree_node_get_next (cell))
    {
      if (i == 0)
        start = cell;
      
      if (cell->parent.widget)
        {
          int min, nat, size;
          gtk_widget_measure (cell->parent.widget, self->orientation,
                              self->column_width,
                              &min, &nat, NULL, NULL);
          if (self->scroll_policy[self->orientation] == GTK_SCROLL_MINIMUM)
            size = min;
          else
            size = nat;
          g_array_append_val (heights, size);
          row_height = MAX (row_height, size);
        }
      cell_set_size (cell, 0);
      i += cell->parent.n_items;

      if (i >= self->n_columns)
        {
          i %= self->n_columns;

          cell_set_size (start, start->size + row_height);
          start = cell;
          row_height = 0;
        }
    }
  if (i > 0)
    cell_set_size (start, start->size + row_height);

  /* step 3: determine height of rows with only unknown items */
  self->unknown_row_height = gtk_grid_view_get_unknown_row_size (self, heights);
  g_array_free (heights, TRUE);

  i = 0;
  known = FALSE;
  for (start = cell = gtk_list_item_manager_get_first (self->item_manager);
       cell != NULL;
       cell = gtk_rb_tree_node_get_next (cell))
    {
      if (i == 0)
        start = cell;

      if (cell->parent.widget)
        known = TRUE;

      i += cell->parent.n_items;
      if (i >= self->n_columns)
        {
          if (!known)
            cell_set_size (start, start->size + self->unknown_row_height);

          i -= self->n_columns;
          known = FALSE;

          if (i >= self->n_columns)
            {
              cell_set_size (cell, cell->size + self->unknown_row_height * (i / self->n_columns));
              i %= self->n_columns;
            }
          start = cell;
        }
    }
  if (i > 0 && !known)
    cell_set_size (start, start->size + self->unknown_row_height);

  /* step 4: update the adjustments */
  x = - gtk_grid_view_update_adjustment (self, opposite_orientation);
  y = - gtk_grid_view_update_adjustment (self, self->orientation);

  i = 0;
  row_height = 0;
  for (cell = gtk_list_item_manager_get_first (self->item_manager);
       cell != NULL;
       cell = gtk_rb_tree_node_get_next (cell))
    {
      if (cell->parent.widget)
        {
          if (i == 0)
            {
              y += row_height;
              row_height = cell->size;
            }
          gtk_grid_view_size_allocate_child (self,
                                             cell->parent.widget,
                                             x + ceil (self->column_width * i),
                                             y,
                                             ceil (self->column_width * (i + 1)) - ceil (self->column_width * i),
                                             row_height);
          i = (i + 1) % self->n_columns;
        }
      else
        {
          i += cell->parent.n_items;
          if (i > self->n_columns)
            {
              i -= self->n_columns;
              y += row_height;
              row_height = cell->size;

              if (i > self->n_columns)
                {
                  guint unknown_rows = (i - 1) / self->n_columns;
                  int unknown_height = unknown_rows * self->unknown_row_height;
                  row_height -= unknown_height;
                  y += unknown_height;
                  i %= self->n_columns;
                }
            }
        }
    }
}

static void
gtk_grid_view_clear_adjustment (GtkGridView    *self,
                                GtkOrientation  orientation)
{
  if (self->adjustment[orientation] == NULL)
    return;

  g_signal_handlers_disconnect_by_func (self->adjustment[orientation],
                                        gtk_grid_view_adjustment_value_changed_cb,
                                        self);
  g_clear_object (&self->adjustment[orientation]);
}

static void
gtk_grid_view_dispose (GObject *object)
{
  GtkGridView *self = GTK_GRID_VIEW (object);

  g_clear_object (&self->model);

  gtk_grid_view_clear_adjustment (self, GTK_ORIENTATION_HORIZONTAL);
  gtk_grid_view_clear_adjustment (self, GTK_ORIENTATION_VERTICAL);

  if (self->anchor)
    {
      gtk_list_item_tracker_free (self->item_manager, self->anchor);
      self->anchor = NULL;
    }
  g_clear_object (&self->item_manager);

  G_OBJECT_CLASS (gtk_grid_view_parent_class)->dispose (object);
}

static void
gtk_grid_view_get_property (GObject    *object,
                            guint       property_id,
                            GValue     *value,
                            GParamSpec *pspec)
{
  GtkGridView *self = GTK_GRID_VIEW (object);

  switch (property_id)
    {
    case PROP_FACTORY:
      g_value_set_object (value, gtk_list_item_manager_get_factory (self->item_manager));
      break;

    case PROP_HADJUSTMENT:
      g_value_set_object (value, self->adjustment[GTK_ORIENTATION_HORIZONTAL]);
      break;

    case PROP_HSCROLL_POLICY:
      g_value_set_enum (value, self->scroll_policy[GTK_ORIENTATION_HORIZONTAL]);
      break;

    case PROP_MAX_COLUMNS:
      g_value_set_uint (value, self->max_columns);
      break;

    case PROP_MIN_COLUMNS:
      g_value_set_uint (value, self->min_columns);
      break;

    case PROP_MODEL:
      g_value_set_object (value, self->model);
      break;

    case PROP_ORIENTATION:
      g_value_set_enum (value, self->orientation);
      break;

    case PROP_VADJUSTMENT:
      g_value_set_object (value, self->adjustment[GTK_ORIENTATION_VERTICAL]);
      break;

    case PROP_VSCROLL_POLICY:
      g_value_set_enum (value, self->scroll_policy[GTK_ORIENTATION_VERTICAL]);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
      break;
    }
}

static void
gtk_grid_view_set_adjustment (GtkGridView    *self,
                              GtkOrientation  orientation,
                              GtkAdjustment  *adjustment)
{
  if (self->adjustment[orientation] == adjustment)
    return;

  if (adjustment == NULL)
    adjustment = gtk_adjustment_new (0.0, 0.0, 0.0, 0.0, 0.0, 0.0);
  g_object_ref_sink (adjustment);

  gtk_grid_view_clear_adjustment (self, orientation);

  self->adjustment[orientation] = adjustment;

  g_signal_connect (adjustment, "value-changed",
		    G_CALLBACK (gtk_grid_view_adjustment_value_changed_cb),
		    self);

  gtk_widget_queue_allocate (GTK_WIDGET (self));
}

static void
gtk_grid_view_set_scroll_policy (GtkGridView         *self,
                                 GtkOrientation       orientation,
                                 GtkScrollablePolicy  scroll_policy)
{
  if (self->scroll_policy[orientation] == scroll_policy)
    return;

  self->scroll_policy[orientation] = scroll_policy;

  gtk_widget_queue_resize (GTK_WIDGET (self));

  g_object_notify_by_pspec (G_OBJECT (self),
                            orientation == GTK_ORIENTATION_HORIZONTAL
                            ? properties[PROP_HSCROLL_POLICY]
                            : properties[PROP_VSCROLL_POLICY]);
}

static void
gtk_grid_view_set_property (GObject      *object,
                            guint         property_id,
                            const GValue *value,
                            GParamSpec   *pspec)
{
  GtkGridView *self = GTK_GRID_VIEW (object);

  switch (property_id)
    {
    case PROP_FACTORY:
      gtk_grid_view_set_factory (self, g_value_get_object (value));
      break;

    case PROP_HADJUSTMENT:
      gtk_grid_view_set_adjustment (self, GTK_ORIENTATION_HORIZONTAL, g_value_get_object (value));
      break;

    case PROP_HSCROLL_POLICY:
      gtk_grid_view_set_scroll_policy (self, GTK_ORIENTATION_HORIZONTAL, g_value_get_enum (value));
      break;

    case PROP_MAX_COLUMNS:
      gtk_grid_view_set_max_columns (self, g_value_get_uint (value));
      break;

    case PROP_MIN_COLUMNS:
      gtk_grid_view_set_min_columns (self, g_value_get_uint (value));
      break;
 
    case PROP_ORIENTATION:
      {
        GtkOrientation orientation = g_value_get_enum (value);
        if (self->orientation != orientation)
          {
            self->orientation = orientation;
            _gtk_orientable_set_style_classes (GTK_ORIENTABLE (self));
            gtk_widget_queue_resize (GTK_WIDGET (self));
            g_object_notify_by_pspec (G_OBJECT (self), properties[PROP_ORIENTATION]);
          }
      }
      break;

    case PROP_MODEL:
      gtk_grid_view_set_model (self, g_value_get_object (value));
      break;

    case PROP_VADJUSTMENT:
      gtk_grid_view_set_adjustment (self, GTK_ORIENTATION_VERTICAL, g_value_get_object (value));
      break;

    case PROP_VSCROLL_POLICY:
      gtk_grid_view_set_scroll_policy (self, GTK_ORIENTATION_VERTICAL, g_value_get_enum (value));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
      break;
    }
}

static void
gtk_grid_view_class_init (GtkGridViewClass *klass)
{
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  gpointer iface;

  widget_class->measure = gtk_grid_view_measure;
  widget_class->size_allocate = gtk_grid_view_size_allocate;

  gobject_class->dispose = gtk_grid_view_dispose;
  gobject_class->get_property = gtk_grid_view_get_property;
  gobject_class->set_property = gtk_grid_view_set_property;

  /* GtkScrollable implementation */
  iface = g_type_default_interface_peek (GTK_TYPE_SCROLLABLE);
  properties[PROP_HADJUSTMENT] =
      g_param_spec_override ("hadjustment",
                             g_object_interface_find_property (iface, "hadjustment"));
  properties[PROP_HSCROLL_POLICY] =
      g_param_spec_override ("hscroll-policy",
                             g_object_interface_find_property (iface, "hscroll-policy"));
  properties[PROP_VADJUSTMENT] =
      g_param_spec_override ("vadjustment",
                             g_object_interface_find_property (iface, "vadjustment"));
  properties[PROP_VSCROLL_POLICY] =
      g_param_spec_override ("vscroll-policy",
                             g_object_interface_find_property (iface, "vscroll-policy"));

  /**
   * GtkGridView:factory:
   *
   * Factory for populating list items
   */
  properties[PROP_FACTORY] =
    g_param_spec_object ("factory",
                         P_("Factory"),
                         P_("Factory for populating list items"),
                         GTK_TYPE_LIST_ITEM_FACTORY,
                         G_PARAM_READWRITE | G_PARAM_EXPLICIT_NOTIFY | G_PARAM_STATIC_STRINGS);


  /**
   * GtkGridView:max-columns:
   *
   * Maximum number of columns per row
   *
   * If this number is smaller than GtkGridView:min-columns, that value
   * is used instead.
   */
  properties[PROP_MAX_COLUMNS] =
    g_param_spec_uint ("max-columns",
                       P_("Max columns"),
                       P_("Maximum number of columns per row"),
                       1, G_MAXUINT, DEFAULT_MAX_COLUMNS,
                       G_PARAM_READWRITE | G_PARAM_EXPLICIT_NOTIFY | G_PARAM_STATIC_STRINGS);

  /**
   * GtkGridView:min-columns:
   *
   * Minimum number of columns per row
   */
  properties[PROP_MIN_COLUMNS] =
    g_param_spec_uint ("min-columns",
                       P_("Min columns"),
                       P_("Minimum number of columns per row"),
                       1, G_MAXUINT, 1,
                       G_PARAM_READWRITE | G_PARAM_EXPLICIT_NOTIFY | G_PARAM_STATIC_STRINGS);

  /**
   * GtkGridView:model:
   *
   * Model for the items displayed
   */
  properties[PROP_MODEL] =
    g_param_spec_object ("model",
                         P_("Model"),
                         P_("Model for the items displayed"),
                         G_TYPE_LIST_MODEL,
                         G_PARAM_READWRITE | G_PARAM_EXPLICIT_NOTIFY | G_PARAM_STATIC_STRINGS);

  /**
   * GtkGridView:orientation:
   *
   * The orientation of the gridview. See GtkOrientable:orientation
   * for details.
   */
  properties[PROP_ORIENTATION] =
    g_param_spec_enum ("orientation",
                       P_("Orientation"),
                       P_("The orientation of the orientable"),
                       GTK_TYPE_ORIENTATION,
                       GTK_ORIENTATION_VERTICAL,
                       GTK_PARAM_READWRITE | G_PARAM_EXPLICIT_NOTIFY);

  g_object_class_install_properties (gobject_class, N_PROPS, properties);

  gtk_widget_class_set_css_name (widget_class, I_("flowbox"));

}

static void
cell_augment (GtkRbTree *tree,
              gpointer   node_augment,
              gpointer   node,
              gpointer   left,
              gpointer   right)
{
  Cell *cell = node;
  CellAugment *aug = node_augment;

  gtk_list_item_manager_augment_node (tree, node_augment, node, left, right);

  aug->size = cell->size;

  if (left)
    {
      CellAugment *left_aug = gtk_rb_tree_get_augment (tree, left);

      aug->size += left_aug->size;
    }

  if (right)
    {
      CellAugment *right_aug = gtk_rb_tree_get_augment (tree, right);

      aug->size += right_aug->size;
    }
}

static void
gtk_grid_view_init (GtkGridView *self)
{
  self->item_manager = gtk_list_item_manager_new (GTK_WIDGET (self), "flowboxchild", Cell, CellAugment, cell_augment);
  self->anchor = gtk_list_item_tracker_new (self->item_manager);

  self->min_columns = 1;
  self->max_columns = DEFAULT_MAX_COLUMNS;
  self->orientation = GTK_ORIENTATION_VERTICAL;

  self->adjustment[GTK_ORIENTATION_HORIZONTAL] = gtk_adjustment_new (0.0, 0.0, 0.0, 0.0, 0.0, 0.0);
  self->adjustment[GTK_ORIENTATION_VERTICAL] = gtk_adjustment_new (0.0, 0.0, 0.0, 0.0, 0.0, 0.0);

  gtk_widget_set_overflow (GTK_WIDGET (self), GTK_OVERFLOW_HIDDEN);
}

/**
 * gtk_grid_view_new:
 *
 * Creates a new empty #GtkGridView.
 *
 * You most likely want to call gtk_grid_view_set_factory() to
 * set up a way to map its items to widgets and gtk_grid_view_set_model()
 * to set a model to provide items next.
 *
 * Returns: a new #GtkGridView
 **/
GtkWidget *
gtk_grid_view_new (void)
{
  return g_object_new (GTK_TYPE_GRID_VIEW, NULL);
}

/**
 * gtk_grid_view_new_with_factory:
 * @factory: (transfer full): The factory to populate items with
 *
 * Creates a new #GtkGridView that uses the given @factory for
 * mapping items to widgets.
 *
 * You most likely want to call gtk_grid_view_set_model() to set
 * a model next.
 *
 * The function takes ownership of the
 * argument, so you can write code like
 * ```
 *   grid_view = gtk_grid_view_new_with_factory (
 *     gtk_builder_list_item_factory_new_from_resource ("/resource.ui"));
 * ```
 *
 * Returns: a new #GtkGridView using the given @factory
 **/
GtkWidget *
gtk_grid_view_new_with_factory (GtkListItemFactory *factory)
{
  GtkWidget *result;

  g_return_val_if_fail (GTK_IS_LIST_ITEM_FACTORY (factory), NULL);

  result = g_object_new (GTK_TYPE_GRID_VIEW,
                         "factory", factory,
                         NULL);

  g_object_unref (factory);

  return result;
}

/**
 * gtk_grid_view_get_model:
 * @self: a #GtkGridView
 *
 * Gets the model that's currently used to read the items displayed.
 *
 * Returns: (nullable) (transfer none): The model in use
 **/
GListModel *
gtk_grid_view_get_model (GtkGridView *self)
{
  g_return_val_if_fail (GTK_IS_GRID_VIEW (self), NULL);

  return self->model;
}

/**
 * gtk_grid_view_set_model:
 * @self: a #GtkGridView
 * @model: (allow-none) (transfer none): the model to use or %NULL for none
 *
 * Sets the #GListModel to use for
 **/
void
gtk_grid_view_set_model (GtkGridView *self,
                         GListModel  *model)
{
  g_return_if_fail (GTK_IS_GRID_VIEW (self));
  g_return_if_fail (model == NULL || G_IS_LIST_MODEL (model));

  if (self->model == model)
    return;

  g_clear_object (&self->model);

  if (model)
    {
      GtkSelectionModel *selection_model;

      self->model = g_object_ref (model);

      if (GTK_IS_SELECTION_MODEL (model))
        selection_model = GTK_SELECTION_MODEL (g_object_ref (model));
      else
        selection_model = GTK_SELECTION_MODEL (gtk_single_selection_new (model));

      gtk_list_item_manager_set_model (self->item_manager, selection_model);
      gtk_grid_view_set_anchor (self, 0, 0.0, TRUE, 0.0, TRUE);

      g_object_unref (selection_model);
    }
  else
    {
      gtk_list_item_manager_set_model (self->item_manager, NULL);
    }

  g_object_notify_by_pspec (G_OBJECT (self), properties[PROP_MODEL]);
}

/**
 * gtk_grid_view_get_factory:
 * @self: a #GtkGridView
 *
 * Gets the factory that's currently used to populate list items.
 *
 * Returns: (nullable) (transfer none): The factory in use
 **/
GtkListItemFactory *
gtk_grid_view_get_factory (GtkGridView *self)
{
  g_return_val_if_fail (GTK_IS_GRID_VIEW (self), NULL);

  return gtk_list_item_manager_get_factory (self->item_manager);
}

/**
 * gtk_grid_view_set_factory:
 * @self: a #GtkGridView
 * @factory: (allow-none) (transfer none): the factory to use or %NULL for none
 *
 * Sets the #GtkListItemFactory to use for populating list items.
 **/
void
gtk_grid_view_set_factory (GtkGridView        *self,
                           GtkListItemFactory *factory)
{
  g_return_if_fail (GTK_IS_GRID_VIEW (self));
  g_return_if_fail (factory == NULL || GTK_LIST_ITEM_FACTORY (factory));

  if (factory == gtk_list_item_manager_get_factory (self->item_manager))
    return;

  gtk_list_item_manager_set_factory (self->item_manager, factory);

  g_object_notify_by_pspec (G_OBJECT (self), properties[PROP_FACTORY]);
}

/**
 * gtk_grid_view_get_max_columns:
 * @self: a #GtkGridView
 *
 * Gets the maximum number of columns that the grid will use.
 *
 * Returns: The maximum number of columns
 **/
guint
gtk_grid_view_get_max_columns (GtkGridView *self)
{
  g_return_val_if_fail (GTK_IS_GRID_VIEW (self), DEFAULT_MAX_COLUMNS);

  return self->max_columns;
}

/**
 * gtk_grid_view_set_max_columns:
 * @self: a #GtkGridView
 * @max_columns: The maximum number of columns
 *
 * Sets the maximum number of columns to use. This number must be at least 1.
 * 
 * If @max_columns is smaller than the minimum set via
 * gtk_grid_view_set_min_columns(), that value is used instead.
 **/
void
gtk_grid_view_set_max_columns (GtkGridView *self,
                               guint        max_columns)
{
  g_return_if_fail (GTK_IS_GRID_VIEW (self));
  g_return_if_fail (max_columns > 0);

  if (self->max_columns == max_columns)
    return;

  self->max_columns = max_columns;

  gtk_grid_view_set_anchor (self,
                            gtk_list_item_tracker_get_position (self->item_manager, self->anchor),
                            self->anchor_xalign,
                            self->anchor_xstart,
                            self->anchor_yalign,
                            self->anchor_ystart);

  gtk_widget_queue_resize (GTK_WIDGET (self));

  g_object_notify_by_pspec (G_OBJECT (self), properties[PROP_MAX_COLUMNS]);
}

/**
 * gtk_grid_view_get_min_columns:
 * @self: a #GtkGridView
 *
 * Gets the minimum number of columns that the grid will use.
 *
 * Returns: The minimum number of columns
 **/
guint
gtk_grid_view_get_min_columns (GtkGridView *self)
{
  g_return_val_if_fail (GTK_IS_GRID_VIEW (self), 1);

  return self->min_columns;
}

/**
 * gtk_grid_view_set_min_columns:
 * @self: a #GtkGridView
 * @min_columns: The minimum number of columns
 *
 * Sets the minimum number of columns to use. This number must be at least 1.
 * 
 * If @min_columns is smaller than the minimum set via
 * gtk_grid_view_set_max_columns(), that value is ignored.
 **/
void
gtk_grid_view_set_min_columns (GtkGridView *self,
                               guint        min_columns)
{
  g_return_if_fail (GTK_IS_GRID_VIEW (self));
  g_return_if_fail (min_columns > 0);

  if (self->min_columns == min_columns)
    return;

  self->min_columns = min_columns;

  gtk_widget_queue_resize (GTK_WIDGET (self));

  g_object_notify_by_pspec (G_OBJECT (self), properties[PROP_MIN_COLUMNS]);
}
