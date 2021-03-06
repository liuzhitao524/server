/*****************************************************************************

Copyright (c) 1996, 2015, Oracle and/or its affiliates. All Rights Reserved.
Copyright (c) 2018, MariaDB Corporation.

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free Software
Foundation; version 2 of the License.

This program is distributed in the hope that it will be useful, but WITHOUT
ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with
this program; if not, write to the Free Software Foundation, Inc.,
51 Franklin Street, Suite 500, Boston, MA 02110-1335 USA

*****************************************************************************/

/**************************************************//**
@file read/read0read.cc
Cursor read

Created 2/16/1997 Heikki Tuuri
*******************************************************/

#include "read0read.h"

#include "srv0srv.h"
#include "trx0sys.h"

/*
-------------------------------------------------------------------------------
FACT A: Cursor read view on a secondary index sees only committed versions
-------
of the records in the secondary index or those versions of rows created
by transaction which created a cursor before cursor was created even
if transaction which created the cursor has changed that clustered index page.

PROOF: We must show that read goes always to the clustered index record
to see that record is visible in the cursor read view. Consider e.g.
following table and SQL-clauses:

create table t1(a int not null, b int, primary key(a), index(b));
insert into t1 values (1,1),(2,2);
commit;

Now consider that we have a cursor for a query

select b from t1 where b >= 1;

This query will use secondary key on the table t1. Now after the first fetch
on this cursor if we do a update:

update t1 set b = 5 where b = 2;

Now second fetch of the cursor should not see record (2,5) instead it should
see record (2,2).

We also should show that if we have delete t1 where b = 5; we still
can see record (2,2).

When we access a secondary key record maximum transaction id is fetched
from this record and this trx_id is compared to up_limit_id in the view.
If trx_id in the record is greater or equal than up_limit_id in the view
cluster record is accessed.  Because trx_id of the creating
transaction is stored when this view was created to the list of
trx_ids not seen by this read view previous version of the
record is requested to be built. This is build using clustered record.
If the secondary key record is delete-marked, its corresponding
clustered record can be already be purged only if records
trx_id < low_limit_no. Purge can't remove any record deleted by a
transaction which was active when cursor was created. But, we still
may have a deleted secondary key record but no clustered record. But,
this is not a problem because this case is handled in
row_sel_get_clust_rec() function which is called
whenever we note that this read view does not see trx_id in the
record. Thus, we see correct version. Q. E. D.

-------------------------------------------------------------------------------
FACT B: Cursor read view on a clustered index sees only committed versions
-------
of the records in the clustered index or those versions of rows created
by transaction which created a cursor before cursor was created even
if transaction which created the cursor has changed that clustered index page.

PROOF:  Consider e.g.following table and SQL-clauses:

create table t1(a int not null, b int, primary key(a));
insert into t1 values (1),(2);
commit;

Now consider that we have a cursor for a query

select a from t1 where a >= 1;

This query will use clustered key on the table t1. Now after the first fetch
on this cursor if we do a update:

update t1 set a = 5 where a = 2;

Now second fetch of the cursor should not see record (5) instead it should
see record (2).

We also should show that if we have execute delete t1 where a = 5; after
the cursor is opened we still can see record (2).

When accessing clustered record we always check if this read view sees
trx_id stored to clustered record. By default we don't see any changes
if record trx_id >= low_limit_id i.e. change was made transaction
which started after transaction which created the cursor. If row
was changed by the future transaction a previous version of the
clustered record is created. Thus we see only committed version in
this case. We see all changes made by committed transactions i.e.
record trx_id < up_limit_id. In this case we don't need to do anything,
we already see correct version of the record. We don't see any changes
made by active transaction except creating transaction. We have stored
trx_id of creating transaction to list of trx_ids when this view was
created. Thus we can easily see if this record was changed by the
creating transaction. Because we already have clustered record we can
access roll_ptr. Using this roll_ptr we can fetch undo record.
We can now check that undo_no of the undo record is less than undo_no of the
trancaction which created a view when cursor was created. We see this
clustered record only in case when record undo_no is less than undo_no
in the view. If this is not true we build based on undo_rec previous
version of the record. This record is found because purge can't remove
records accessed by active transaction. Thus we see correct version. Q. E. D.
-------------------------------------------------------------------------------
FACT C: Purge does not remove any delete-marked row that is visible
-------
in any cursor read view.

PROOF: We know that:
 1: Currently active read views in trx_sys_t::view_list are ordered by
    ReadView::low_limit_no in descending order, that is,
    newest read view first.

 2: Purge clones the oldest read view and uses that to determine whether there
    are any active transactions that can see the to be purged records.

Therefore any joining or active transaction will not have a view older
than the purge view, according to 1.

When purge needs to remove a delete-marked row from a secondary index,
it will first check that the DB_TRX_ID value of the corresponding
record in the clustered index is older than the purge view. It will
also check if there is a newer version of the row (clustered index
record) that is not delete-marked in the secondary index. If such a
row exists and is collation-equal to the delete-marked secondary index
record then purge will not remove the secondary index record.

Delete-marked clustered index records will be removed by
row_purge_remove_clust_if_poss(), unless the clustered index record
(and its DB_ROLL_PTR) has been updated. Every new version of the
clustered index record will update DB_ROLL_PTR, pointing to a new UNDO
log entry that allows the old version to be reconstructed. The
DB_ROLL_PTR in the oldest remaining version in the old-version chain
may be pointing to garbage (an undo log record discarded by purge),
but it will never be dereferenced, because the purge view is older
than any active transaction.

For details see: row_vers_old_has_index_entry() and row_purge_poss_sec()

Some additional issues:

What if trx_sys.view_list == NULL and some transaction T1 and Purge both
try to open read_view at same time. Only one can acquire trx_sys.mutex.
In which order will the views be opened? Should it matter? If no, why?

The order does not matter. No new transactions can be created and no running
RW transaction can commit or rollback (or free views). AC-NL-RO transactions
will mark their views as closed but not actually free their views.
*/

/** Minimum number of elements to reserve in ReadView::ids_t */
static const ulint MIN_TRX_IDS = 32;

#ifdef UNIV_DEBUG
/** Functor to validate the view list. */
struct	ViewCheck {

	ViewCheck() : m_prev_view() { }

	void	operator()(const ReadView* view)
	{
		ut_ad(view->is_registered());
		ut_a(m_prev_view == NULL
		     || !view->is_open()
		     || view->le(m_prev_view));

		m_prev_view = view;
	}

	const ReadView*	m_prev_view;
};

/**
Validates a read view list. */

bool
MVCC::validate() const
{
	ViewCheck	check;

	ut_ad(mutex_own(&trx_sys.mutex));

	ut_list_map(m_views, check);

	return(true);
}
#endif /* UNIV_DEBUG */

/**
Try and increase the size of the array. Old elements are
copied across.
@param n 		Make space for n elements */

void
ReadView::ids_t::reserve(ulint n)
{
	if (n <= capacity()) {
		return;
	}

	/** Keep a minimum threshold */
	if (n < MIN_TRX_IDS) {
		n = MIN_TRX_IDS;
	}

	value_type*	p = m_ptr;

	m_ptr = UT_NEW_ARRAY_NOKEY(value_type, n);

	m_reserved = n;

	ut_ad(size() < capacity());

	if (p != NULL) {

		::memmove(m_ptr, p, size() * sizeof(value_type));

		UT_DELETE_ARRAY(p);
	}
}

/**
Copy and overwrite this array contents
@param start		Source array
@param end		Pointer to end of array */

void
ReadView::ids_t::assign(const value_type* start, const value_type* end)
{
	ut_ad(end >= start);

	ulint	n = end - start;

	/* No need to copy the old contents across during reserve(). */
	clear();

	/* Create extra space if required. */
	reserve(n);

	resize(n);

	ut_ad(size() == n);

	::memmove(m_ptr, start, size() * sizeof(value_type));
}

/**
Append a value to the array.
@param value		the value to append */

void
ReadView::ids_t::push_back(value_type value)
{
	if (capacity() <= size()) {
		reserve(size() * 2);
	}

	m_ptr[m_size++] = value;
	ut_ad(size() <= capacity());
}

/**
Insert the value in the correct slot, preserving the order. Doesn't
check for duplicates. */

void
ReadView::ids_t::insert(value_type value)
{
	ut_ad(value > 0);

	reserve(size() + 1);

	if (empty() || back() < value) {
		push_back(value);
		return;
	}

	value_type*	end = data() + size();
	value_type*	ub = std::upper_bound(data(), end, value);

	if (ub == end) {
		push_back(value);
	} else {
		ut_ad(ub < end);

		ulint	n_elems = std::distance(ub, end);
		ulint	n = n_elems * sizeof(value_type);

		/* Note: Copying overlapped memory locations. */
		::memmove(ub + 1, ub, n);

		*ub = value;

		resize(size() + 1);
	}
}

/**
Copy the transaction ids from the source vector */

void
ReadView::copy_trx_ids(const trx_ids_t& trx_ids)
{
	ut_ad(mutex_own(&trx_sys.mutex));
	ulint	size = trx_ids.size();

	if (m_creator_trx_id > 0) {
		ut_ad(size > 0);
		--size;
	}

	if (size == 0) {
		m_ids.clear();
		return;
	}

	m_ids.reserve(size);
	m_ids.resize(size);

	ids_t::value_type*	p = m_ids.data();

	/* Copy all the trx_ids except the creator trx id */

	if (m_creator_trx_id > 0) {

		/* Note: We go through all this trouble because it is
		unclear whether std::vector::resize() will cause an
		overhead or not. We should test this extensively and
		if the vector to vector copy is fast enough then get
		rid of this code and replace it with more readable
		and obvious code. The code below does exactly one copy,
		and filters out the creator's trx id. */

		trx_ids_t::const_iterator	it = std::lower_bound(
			trx_ids.begin(), trx_ids.end(), m_creator_trx_id);

		ut_ad(it != trx_ids.end() && *it == m_creator_trx_id);

		ulint	i = std::distance(trx_ids.begin(), it);
		ulint	n = i * sizeof(trx_ids_t::value_type);

		::memmove(p, &trx_ids[0], n);

		n = (trx_ids.size() - i - 1) * sizeof(trx_ids_t::value_type);

		ut_ad(i + (n / sizeof(trx_ids_t::value_type)) == m_ids.size());

		if (n > 0) {
			::memmove(p + i, &trx_ids[i + 1], n);
		}
	} else {
		ulint	n = size * sizeof(trx_ids_t::value_type);

		::memmove(p, &trx_ids[0], n);
	}

	m_up_limit_id = m_ids.front();

#ifdef UNIV_DEBUG
	/* Original assertion was here to make sure that rw_trx_ids and
	rw_trx_hash are in sync and they hold either ACTIVE or PREPARED
	transaction.

	Now rw_trx_hash.find() does
	ut_ad(trx_state_eq(trx, TRX_STATE_ACTIVE) ||
	      trx_state_eq(trx, TRX_STATE_PREPARED)).
	No need to repeat it here. We even can't repeat it here: it'll be race
	condition because we need trx->element->mutex locked to perform this
	check (see how it is done in find()).

	Now rw_trx_ids and rw_trx_hash may get out of sync for a short while:
	when transaction is registered it first gets added into rw_trx_ids
	under trx_sys.mutex protection and then to rw_trx_hash without mutex
	protection. Thus we need repeat this lookup. */
	for (trx_ids_t::const_iterator it = trx_ids.begin();
	     it != trx_ids.end(); ++it) {
		while (!trx_sys.rw_trx_hash.find(*it));
	}
#endif /* UNIV_DEBUG */
}

/**
Opens a read view where exactly the transactions serialized before this
point in time are seen in the view.
@param id		Creator transaction id */

void
ReadView::prepare(trx_id_t id)
{
	ut_ad(mutex_own(&trx_sys.mutex));

	m_creator_trx_id = id;

	m_low_limit_no = m_low_limit_id = m_up_limit_id =
						trx_sys.get_max_trx_id();

	if (!trx_sys.rw_trx_ids.empty()) {
		copy_trx_ids(trx_sys.rw_trx_ids);
	} else {
		m_ids.clear();
	}

	ut_ad(m_up_limit_id <= m_low_limit_id);

	if (UT_LIST_GET_LEN(trx_sys.serialisation_list) > 0) {
		const trx_t*	trx;

		trx = UT_LIST_GET_FIRST(trx_sys.serialisation_list);

		if (trx->no < m_low_limit_no) {
			m_low_limit_no = trx->no;
		}
	}
}


/**
  Create a view.

  Assigns a read view for a consistent read query. All the consistent reads
  within the same transaction will get the same read view, which is created
  when this function is first called for a new started transaction.

  @param trx   transaction instance of caller
*/

void MVCC::view_open(trx_t* trx)
{
  if (srv_read_only_mode)
  {
    ut_ad(!trx->read_view.is_open());
    return;
  }
  else if (trx->read_view.is_open())
    return;

  /*
    Reuse closed view if there were no read-write transactions since (and at) it's
    creation time.
  */
  if (trx->read_view.is_registered() &&
      trx_is_autocommit_non_locking(trx) &&
      trx->read_view.empty() &&
      trx->read_view.m_low_limit_id == trx_sys.get_max_trx_id())
  {
    /*
      Original comment states: there is an inherent race here between purge
      and this thread.

      To avoid this race we should've checked trx_sys.get_max_trx_id() and
      do trx->read_view.set_open(true) atomically under trx_sys.mutex
      protection. But we're cutting edges to achieve great scalability.

      There're at least two types of concurrent threads interested in this
      value: purge coordinator thread (see MVCC::clone_oldest_view()) and
      InnoDB monitor thread (see lock_trx_print_wait_and_mvcc_state()).

      What bad things can happen because we allow this race?

      First, purge thread may be affected by this race condition only if this
      view is the oldest open view. In other words this view is either last in
      m_views list or there're no open views beyond.

      In this case purge may not catch this view and clone some younger view
      instead. It might be kind of alright, because there were no read-write
      transactions and there should be nothing to purge. Besides younger view
      must have exactly the same values.

      Second, scary things start when there's a read-write transaction starting
      concurrently.

      Speculative execution may reorder set_open() before get_max_trx_id(). In
      this case purge thread has short gap to clone outdated view. Which is
      probably not that bad: it just won't be able to purge things that it was
      actually allowed to purge for a short while.

      This thread may as well get suspended after trx_sys.get_max_trx_id() and
      before trx->read_view.set_open(true). New read-write transaction may get
      started, committed and purged meanwhile. It is acceptable as well, since
      this view doesn't see it.
    */
    trx->read_view.set_open(true);
    return;
  }

  mutex_enter(&trx_sys.mutex);
  trx->read_view.prepare(trx->id);
  if (trx->read_view.is_registered())
    UT_LIST_REMOVE(m_views, &trx->read_view);
  else
    trx->read_view.set_registered(true);
  trx->read_view.set_open(true);
  UT_LIST_ADD_FIRST(m_views, &trx->read_view);
  ut_ad(validate());
  mutex_exit(&trx_sys.mutex);
}

/**
Copy state from another view. Must call copy_complete() to finish.
@param other		view to copy from */

void
ReadView::copy_prepare(const ReadView& other)
{
	ut_ad(&other != this);

	if (!other.m_ids.empty()) {
		const ids_t::value_type* 	p = other.m_ids.data();

		m_ids.assign(p, p + other.m_ids.size());
	} else {
		m_ids.clear();
	}

	m_up_limit_id = other.m_up_limit_id;

	m_low_limit_no = other.m_low_limit_no;

	m_low_limit_id = other.m_low_limit_id;

	m_creator_trx_id = other.m_creator_trx_id;
}

/**
Complete the copy, insert the creator transaction id into the
m_ids too and adjust the m_up_limit_id, if required */

void
ReadView::copy_complete()
{
	ut_ad(!mutex_own(&trx_sys.mutex));

	if (m_creator_trx_id > 0) {
		m_ids.insert(m_creator_trx_id);
	}

	if (!m_ids.empty()) {
		/* The last active transaction has the smallest id. */
		m_up_limit_id = std::min(m_ids.front(), m_up_limit_id);
	}

	ut_ad(m_up_limit_id <= m_low_limit_id);

	/* We added the creator transaction ID to the m_ids. */
	m_creator_trx_id = 0;
}

/** Clones the oldest view and stores it in view. No need to
call view_close(). The caller owns the view that is passed in.
This function is called by Purge to determine whether it should
purge the delete marked record or not.
@param view		Preallocated view, owned by the caller */

void
MVCC::clone_oldest_view(ReadView* view)
{
	mutex_enter(&trx_sys.mutex);
	/* Find oldest view. */
	for (ReadView *oldest_view = UT_LIST_GET_LAST(m_views);
	     oldest_view != NULL;
	     oldest_view = UT_LIST_GET_PREV(m_view_list, oldest_view))
	{
		if (oldest_view->is_open())
		{
			view->copy_prepare(*oldest_view);
			mutex_exit(&trx_sys.mutex);
			view->copy_complete();
			return;
		}
	}
	/* No views in the list: snapshot current state. */
	view->prepare(0);
	mutex_exit(&trx_sys.mutex);
}

/**
@return the number of active views */

size_t
MVCC::size() const
{
	mutex_enter(&trx_sys.mutex);

	size_t	size = 0;

	for (const ReadView* view = UT_LIST_GET_FIRST(m_views);
	     view != NULL;
	     view = UT_LIST_GET_NEXT(m_view_list, view)) {

		if (view->is_open()) {
			++size;
		}
	}

	mutex_exit(&trx_sys.mutex);

	return(size);
}
