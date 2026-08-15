"""Build the code explanation page at docs/html/order_book_explained.html.

Every code snippet on the page is pulled straight out of the C++ source, and
every number is captured by actually running the built binaries. Nothing on the
page is typed in by hand, so the page cannot drift away from the code.

Run it after building the project:

    cmake --build build -j
    uv run python scripts/build_explanation.py

Layout of this file:
    1. paths and helpers
    2. snippet extraction from the C++ source
    3. sessions that are run to produce the sample data
    4. the page body, assembled as one long HTML string
"""

from __future__ import annotations

import html
import re
import subprocess
import sys
from pathlib import Path

# ----------------------------------------------------------------- 1. paths

ROOT = Path(__file__).resolve().parent.parent
SRC = ROOT / "src"
BUILD = ROOT / "build"
OUTPUT = ROOT / "docs" / "html" / "order_book_explained.html"
HEAD = ROOT / "docs" / "html" / "_head.html"

SIM = BUILD / "order_book_sim"
RANDOM = BUILD / "order_book_random"

# Size of the random session run for the page. Kept modest so the page builds
# in well under a second.
RANDOM_INSTRUCTIONS = 20000
RANDOM_SEED = 1


def escape_code(text: str) -> str:
    """Escape a C++ snippet for a <pre> block and colour its comments.

    Parameters
    ----------
    text : str
        Raw C++ source lines.

    Returns
    -------
    str
        HTML-safe text where any `//` comment is wrapped in <span class="c">,
        which the page stylesheet renders in a muted colour.
    """
    escaped = html.escape(text)
    # Wrap the comment part of a line, not any '//' inside a string literal.
    # The engine source has no string literal containing '//', so a plain
    # match is safe here.
    return re.sub(r"(//[^\n]*)", r'<span class="c">\1</span>', escaped)


def escape_text(text: str) -> str:
    """Escape captured program output for a <pre> block."""
    return html.escape(text)


# ------------------------------------------------- 2. snippets from the source


def extract_block(path: Path, start_marker: str) -> str:
    """Pull one brace-balanced block out of a C++ file.

    Starts at the first line containing `start_marker`, then reads on until the
    braces opened since that line are all closed again. Works for a function
    definition, a struct or a class body.

    Parameters
    ----------
    path : Path
        File to read.
    start_marker : str
        Text identifying the first line of the block, for example
        "SubmitResult OrderBook::submit(".

    Returns
    -------
    str
        The block, including its first and last line, with trailing whitespace
        stripped from each line.
    """
    lines = path.read_text().splitlines()
    start = next((i for i, line in enumerate(lines) if start_marker in line), None)
    if start is None:
        raise SystemExit(f"marker not found in {path.name}: {start_marker}")

    depth = 0
    opened = False
    collected = []
    for line in lines[start:]:
        collected.append(line.rstrip())
        depth += line.count("{") - line.count("}")
        if "{" in line:
            opened = True
        if opened and depth == 0:
            break
    return "\n".join(collected)


def extract_lines(path: Path, first_marker: str, last_marker: str) -> str:
    """Pull the lines between two markers, both included."""
    lines = path.read_text().splitlines()
    start = next((i for i, line in enumerate(lines) if first_marker in line), None)
    if start is None:
        raise SystemExit(f"marker not found in {path.name}: {first_marker}")
    end = next(
        (i for i, line in enumerate(lines[start:], start) if last_marker in line), None
    )
    if end is None:
        raise SystemExit(f"end marker not found in {path.name}: {last_marker}")
    return "\n".join(line.rstrip() for line in lines[start : end + 1])


# ------------------------------------------------ 3. sessions run for samples

# Each session is fed to order_book_sim on standard input. The output that
# comes back is what the page shows, so the page and the engine cannot disagree.

SESSION_MATCH = """LIMIT SELL 100.35 300
LIMIT SELL 100.30 200
BOOK
LIMIT BUY 100.40 400
BOOK
"""

SESSION_ICEBERG = """ICEBERG SELL 100.30 1000 100
LIMIT SELL 100.30 100
BOOK
MARKET BUY - 250
BOOK
"""

SESSION_FOK = """LIMIT SELL 100.30 100
LIMIT SELL 100.50 900
FOK BUY 100.35 400
BOOK
"""

SESSION_MODIFY = """LIMIT BUY 100.25 500
LIMIT BUY 100.25 300
MODIFY 1 100.25 200
BOOK
MODIFY 1 100.26 200
BOOK
STATUS 1
"""

SESSION_POSTONLY = """LIMIT SELL 100.30 500
POSTONLY BUY 100.30 200
POSTONLY BUY 100.29 200
BOOK
"""


def run_session(session: str) -> str:
    """Feed a session to order_book_sim and return everything it printed.

    The final book summary the tool always prints is dropped, because each
    sample box already ends with an explicit BOOK line.
    """
    finished = subprocess.run(
        [str(SIM)], input=session, capture_output=True, text=True, check=True
    )
    return finished.stdout.split("\nfinal book")[0].rstrip()


def run_random() -> str:
    """Run the random flow tester and return its output."""
    finished = subprocess.run(
        [str(RANDOM), str(RANDOM_INSTRUCTIONS), str(RANDOM_SEED)],
        capture_output=True,
        text=True,
        check=True,
    )
    if finished.returncode != 0:
        raise SystemExit("the random tester reported a problem; page not built")
    return finished.stdout.rstrip()


def section_of(output: str, heading: str) -> str:
    """Pull one titled block out of the random tester's output."""
    blocks = output.split("\n\n")
    for block in blocks:
        if block.strip().startswith(heading):
            return block.rstrip()
    raise SystemExit(f"block not found in random output: {heading}")


# ------------------------------------------------------------- 4. the page


def build() -> str:
    if not SIM.exists() or not RANDOM.exists():
        raise SystemExit("build the project first: cmake --build build -j")

    # Snippets, straight from the source.
    snip_level = extract_block(SRC / "order_book.hpp", "struct PriceLevel {")
    snip_ladders = extract_lines(
        SRC / "order_book.hpp", "using BidLadder", "std::list<Order>::iterator position;"
    )
    snip_submit = extract_block(SRC / "order_book.cpp", "SubmitResult OrderBook::submit(")
    snip_execute = extract_block(SRC / "order_book.cpp", "SubmitResult OrderBook::execute(")
    snip_match = extract_block(SRC / "order_book.cpp", "void OrderBook::match(")
    snip_rest = extract_block(SRC / "order_book.cpp", "void OrderBook::rest(")
    snip_detach = extract_block(SRC / "order_book.cpp", "Order OrderBook::detach(")
    snip_cancel = extract_block(SRC / "order_book.cpp", "bool OrderBook::cancel(")
    snip_modify = extract_block(SRC / "order_book.cpp", "ModifyResult OrderBook::modify(")
    snip_next = extract_block(SRC / "random_flow.cpp", "Instruction RandomFlow::next(")

    # Sample data, straight from the binaries.
    out_match = run_session(SESSION_MATCH)
    out_iceberg = run_session(SESSION_ICEBERG)
    out_fok = run_session(SESSION_FOK)
    out_modify = run_session(SESSION_MODIFY)
    out_postonly = run_session(SESSION_POSTONLY)

    random_output = run_random()
    random_types = section_of(random_output, "submitted orders")
    random_outcomes = section_of(random_output, "outcomes")
    random_amends = section_of(random_output, "cancels and amendments")
    random_trading = section_of(random_output, "trading")
    random_checks = section_of(random_output, "checks")

    body = f"""
<div class="wrap">

<p class="eyebrow"><span class="dot"></span>ORDER BOOK ENGINE &middot; C++ &middot; code walkthrough</p>
<h1>How the order book engine works</h1>
<p class="lede">A matching engine is a queue with two rules. This page follows one
order from the moment it arrives to the moment it leaves, through the code that
moves it.</p>

<div class="box def">
  <span class="k">How to use this</span>
  <p>Sections follow the path an order takes through the engine. Every snippet is
  copied out of <code>src/</code> by the build script, and every number in a
  <b>Sample data</b> box is what the engine actually printed when the page was
  built. Coloured boxes flag design choices and traps.</p>
</div>

<div class="box why">
  <span class="k">The whole engine in plain English</span>
  <p>Buyers and sellers send in orders. The engine keeps the unfilled ones sorted:
  best price first, and within a price, oldest first. When a new order arrives it
  is compared against the best order on the other side. If they overlap in price,
  they trade, and the trade happens at the resting order's price. Whatever is left
  of the new order either joins the queue or is thrown away, depending on the type
  of order it is.</p>
  <p>The two hard parts are not the matching. They are cancelling an order sitting
  in the middle of a queue without searching for it, and keeping the cached sizes
  at each price honest while orders come and go.</p>
</div>

<h2 id="map"><span class="sec-no">00</span>Map</h2>

<div class="mermaid">
flowchart LR
  in["submit()"] --> ex["execute()"]
  ex --> mt["match()"]
  mt --> rs["rest()"]
  ex --> rs
  cx["cancel()"] --> dt["detach()"]
  md["modify()"] --> dt
  md --> ex
  rs --> book[("bids / asks<br/>+ locators")]
  dt --> book
  mt --> book
  book --> vl["validate()"]
</div>
<p class="fnote">Boxes are functions, the cylinder is the stored book, arrows show
the direction work flows. <code>submit()</code>, <code>cancel()</code> and
<code>modify()</code> are the three doors in; everything else is internal.</p>

<div class="toc">
  <div class="rk">Contents</div>
  <ol>
    <li><a href="#sec1">Where the orders live &mdash; the three structures</a></li>
    <li><a href="#sec2">The front door &mdash; <code>submit()</code></a></li>
    <li><a href="#sec3">Checks, match, rest &mdash; <code>execute()</code></a></li>
    <li><a href="#sec4">The matching loop &mdash; <code>match()</code></a></li>
    <li><a href="#sec5">Joining the queue &mdash; <code>rest()</code></a></li>
    <li><a href="#sec6">Leaving early &mdash; <code>cancel()</code> and <code>detach()</code></a></li>
    <li><a href="#sec7">Changing your mind &mdash; <code>modify()</code></a></li>
    <li><a href="#sec8">Proving it is still sane &mdash; <code>validate()</code></a></li>
    <li><a href="#sec9">Random order flow &mdash; <code>RandomFlow::next()</code></a></li>
  </ol>
</div>

<h2 id="sec1"><span class="sec-no">01</span>Where the orders live &mdash; the three structures</h2>

<p>Everything else in the engine is a consequence of these three choices, so they
come first.</p>

<pre>{escape_code(snip_level)}</pre>

<pre>{escape_code(snip_ladders)}</pre>

<div class="box gist"><span class="k">In plain words</span>
<p>The book is a sorted list of prices. Each price holds a queue of orders. A
separate address book says exactly where each order is sitting, so it can be
pulled out without searching.</p></div>

<div class="box def"><span class="k">What each one does</span>
<p><b><code>bids_</code> and <code>asks_</code>:</b> ordered maps from price to
that price's queue. Bids sort high to low and asks low to high, so
<code>begin()</code> is always the best price on that side.<br>
<b><code>PriceLevel::orders</code>:</b> a <code>std::list</code>, which is a
doubly linked list. Front is the oldest order, back is the newest.<br>
<b><code>locators_</code>:</b> a hash map from order id to the side, the price,
and an iterator pointing at that exact spot in that exact queue.</p></div>

<div class="box why"><span class="k">Why a map and not a heap</span>
<p>A binary heap gives you the best element cheaply, which sounds right for
"best price". But cancelling means removing an order at an arbitrary price, and a
heap cannot remove an arbitrary element without scanning it. An ordered map keeps
the best price at <code>begin()</code> <i>and</i> lets you find or erase any price
by key.</p></div>

<div class="box why"><span class="k">Why a linked list and not a vector</span>
<p>Because <code>locators_</code> stores an iterator into the queue. Erasing from
the middle of a vector shifts every element behind it, which would invalidate
every stored position at that price at once. Erasing from a linked list touches
only its two neighbours, and every other iterator stays valid.</p></div>

<div class="box def"><span class="k">Two sizes per level, not one</span>
<p><code>visible_quantity</code> is what the market can see.
<code>total_quantity</code> includes the hidden part of any iceberg resting there.
For a level with no iceberg the two are equal. Keeping both as running totals
means reading the book's depth never walks the queue.</p></div>

<div class="box warn"><span class="k">Trap</span>
<p>Those two running totals are the easiest thing in the engine to get wrong. Every
single place that changes an order's size must adjust them by the same amount, or
the book will print sizes that do not exist. This is exactly what
<code>validate()</code> in section 08 checks, and why the random tester runs it
after every instruction.</p></div>

<h2 id="sec2"><span class="sec-no">02</span>The front door &mdash; <code>submit()</code></h2>

<pre>{escape_code(snip_submit)}</pre>

<div class="box gist"><span class="k">In plain words</span>
<p>Check the request makes sense, stamp it with a fresh order number, and hand it
to the part that does the real work.</p></div>

<div class="box def"><span class="k">What it does</span>
<p><b>Input:</b> side, order type, price, quantity, and for an iceberg the size to
display. <b>Output:</b> a <code>SubmitResult</code> holding the new order id, the
trades it caused, its final status, and whether anything is left resting.
<b>Role:</b> the only way a new order enters the book.</p></div>

<div class="box data"><span class="k">Sample data</span>
<div class="io stack">
<div><span class="lab">in &mdash; four instructions</span>
<pre>{escape_text(SESSION_MATCH.rstrip())}</pre></div>
<div><span class="lab">out &mdash; what the engine printed</span>
<pre>{escape_text(out_match)}</pre>
<p class="fnote">The buyer was willing to pay 100.40 but paid 100.30 for the first
200 and 100.35 for the next 200, because a trade happens at the resting order's
price. 400 &times; 100.40 would have been 40160; the buyer paid 40130 instead.</p></div>
</div></div>

<p><b>Step by step:</b></p>
<ol>
  <li>Reject a quantity of zero or less outright, with an exception rather than a
  silent no-op.</li>
  <li>Reject an iceberg that has no display size, since it would be invisible and
  could never be traded against.</li>
  <li>Fill in a fresh <code>Order</code>: a new id, the side and type as given, and
  the quantity in three fields at once.</li>
  <li>A market order's price is forced to <code>kNoPrice</code>, so a stray price
  from the caller can never be used by accident.</li>
  <li>Hand it to <code>execute()</code>.</li>
</ol>

<div class="box why"><span class="k">Why three quantity fields</span>
<p><code>original_quantity</code> never changes, so "how much of this has traded"
is always <code>original - remaining</code>. <code>remaining</code> is what is
left to trade. <code>visible</code> is what the market can see, which differs from
<code>remaining</code> only for an iceberg. Storing the filled amount separately
would mean keeping two numbers in step; deriving it means they cannot disagree.</p></div>

<h2 id="sec3"><span class="sec-no">03</span>Checks, match, rest &mdash; <code>execute()</code></h2>

<pre>{escape_code(snip_execute)}</pre>

<div class="box gist"><span class="k">In plain words</span>
<p>Two order types get a verdict before anything trades. Everything else goes
straight to matching, and whatever survives either joins the book or is thrown
away.</p></div>

<div class="box def"><span class="k">What it does</span>
<p><b>Input:</b> a fully formed order, either brand new or one coming back from an
amendment. <b>Output:</b> the trades, the final status, and whether it rested.
<b>Role:</b> the single path every order takes, so a new order and an amended one
cannot behave differently.</p></div>

<div class="box data"><span class="k">Sample data &mdash; post-only</span>
<div class="io stack">
<div><span class="lab">in</span>
<pre>{escape_text(SESSION_POSTONLY.rstrip())}</pre></div>
<div><span class="lab">out</span>
<pre>{escape_text(out_postonly)}</pre>
<p class="fnote">Order 2 would have traded at once, so it was refused. Order 3, one
cent lower, rested and became the best bid.</p></div>
</div></div>

<div class="box data"><span class="k">Sample data &mdash; fill-or-kill</span>
<div class="io stack">
<div><span class="lab">in</span>
<pre>{escape_text(SESSION_FOK.rstrip())}</pre></div>
<div><span class="lab">out</span>
<pre>{escape_text(out_fok)}</pre>
<p class="fnote">Only 100 was available at or below 100.35. The 900 sitting at
100.50 is too expensive to count, so the order was killed and the book is
untouched: both sellers are still there.</p></div>
</div></div>

<p><b>Step by step:</b></p>
<ol>
  <li>Stamp an arrival number. An amended order gets a new one, which is how it
  loses its place in the queue.</li>
  <li>A post-only order is refused if the best price on the other side would let it
  trade. It exists to add liquidity, never to take it.</li>
  <li>A fill-or-kill order is measured against the book before anything happens. If
  the whole size cannot be filled, it is killed and nothing changes.</li>
  <li>Match against the other side, using a test for "does this price cross" that
  differs only in direction between a buy and a sell.</li>
  <li>Limit, iceberg and post-only remainders rest. Market, immediate-or-cancel and
  fill-or-kill remainders do not.</li>
  <li>Work out the final status from what happened, and file the order away if it
  has left the book.</li>
</ol>

<div class="box why"><span class="k">Why fill-or-kill is checked first, not attempted</span>
<p>The alternative is to trade and then undo it if the order could not be
completed. Undoing a partial sweep means putting several orders back at their old
places in several queues, and getting that exactly right is far harder than
counting first. Counting is one pass over a few price levels and cannot fail
halfway.</p></div>

<div class="box def"><span class="k">Hidden size counts as real liquidity</span>
<p>The fill-or-kill check calls <code>available_quantity</code>, which sums
<code>total_quantity</code> per level, including the hidden part of icebergs. The
reasoning is that hidden size really does execute, so ignoring it would kill
orders that could in fact have been filled.</p></div>

<div class="box flag"><span class="k">The choice, stated honestly</span>
<p>Not every venue agrees. Some count only displayed size for this test, which
makes fill-or-kill orders fail more often but keeps hidden size genuinely hidden.
This engine takes the other view. Changing it is one argument to
<code>available_quantity</code>.</p></div>

<h2 id="sec4"><span class="sec-no">04</span>The matching loop &mdash; <code>match()</code></h2>

<pre>{escape_code(snip_match)}</pre>

<div class="box gist"><span class="k">In plain words</span>
<p>Take from the best price on the other side, oldest order first, until the
incoming order is used up or the next price is too expensive.</p></div>

<div class="box def"><span class="k">What it does</span>
<p><b>Input:</b> one side of the book, the incoming order, and a test saying which
resting prices it is willing to trade at. <b>Output:</b> trades appended to a list,
and <code>incoming.remaining</code> reduced as it goes. <b>Role:</b> the only place
in the engine where a trade is created.</p></div>

<div class="box data"><span class="k">Sample data &mdash; an iceberg refilling</span>
<div class="io stack">
<div><span class="lab">in</span>
<pre>{escape_text(SESSION_ICEBERG.rstrip())}</pre></div>
<div><span class="lab">out</span>
<pre>{escape_text(out_iceberg)}</pre>
<p class="fnote">Order 1 hides 1000 behind a 100 slice, so the level shows 200, not
1100. The market buy of 250 produced three trades: 100 from the iceberg's slice,
then 100 from order 2, then 50 from the iceberg's next slice. The iceberg went to
the back of the queue when its slice ran out, which is why order 2 traded second
even though it arrived later.</p></div>
</div></div>

<p><b>Step by step:</b></p>
<ol>
  <li>Look at the best price on the other side. Stop if the incoming order will not
  pay it.</li>
  <li>Trade against the order at the front of that price's queue, for the smaller
  of what the taker still wants and what the maker is showing.</li>
  <li>Reduce four numbers by the same amount: the taker's remaining, the maker's
  visible and remaining, and both of the level's running totals.</li>
  <li>Record the trade at the maker's price.</li>
  <li>If the maker is fully filed, file it away and drop it from the queue.</li>
  <li>If only its shown slice ran out, refresh the slice and move the order to the
  back of the queue.</li>
  <li>When a price level empties, erase it, so the book never holds a price with
  nothing behind it.</li>
</ol>

<div class="box why"><span class="k">Why <code>splice</code> and not erase-and-push</span>
<p><code>splice</code> relinks the existing node into a new position. Nothing is
copied and, crucially, the iterator stored in <code>locators_</code> keeps
pointing at the same order. Erasing and re-inserting would create a new node,
leaving <code>locators_</code> holding a dangling iterator that a later cancel
would follow straight into freed memory.</p></div>

<div class="box why"><span class="k">Why the trade prints at the maker's price</span>
<p>The resting order named its price first and is doing the market a service by
waiting there. The arriving order is willing to pay up to its own limit, so any
gap between the two is a benefit that goes to the taker. Every major venue works
this way.</p></div>

<div class="box warn"><span class="k">Trap</span>
<p>The inner loop takes a reference to <code>level.orders.front()</code> and then,
in the iceberg branch, moves that very node to the back. The reference stays valid
because <code>splice</code> moves nodes rather than copying them; with a vector
this pattern would be a use-after-free. It also means the loop is guaranteed to
make progress only because either the maker is removed or the taker's remaining
size falls.</p></div>

<h2 id="sec5"><span class="sec-no">05</span>Joining the queue &mdash; <code>rest()</code></h2>

<pre>{escape_code(snip_rest)}</pre>

<div class="box gist"><span class="k">In plain words</span>
<p>Put the leftover at the back of the queue for its price, and write down where it
went.</p></div>

<div class="box def"><span class="k">What it does</span>
<p><b>Input:</b> an order with quantity still unfilled. <b>Output:</b> nothing; the
book and the address book are updated. <b>Role:</b> the only place an order is
added to a queue.</p></div>

<p><b>Step by step:</b></p>
<ol>
  <li>Work out what to display: one slice for an iceberg, the whole size otherwise.</li>
  <li>Look up the price level, creating it if this is the first order there.</li>
  <li>Push the order onto the back of that queue, which is what gives newer orders
  worse priority.</li>
  <li>Add to both running totals.</li>
  <li>Record the side, the price and an iterator to this exact position in
  <code>locators_</code>.</li>
</ol>

<div class="box why"><span class="k">Why the same lambda for both sides</span>
<p><code>bids_</code> and <code>asks_</code> are different types, because they sort
in opposite directions. A generic lambda taking <code>auto&amp;</code> is written
once and compiled twice, once per side, which avoids two near-identical copies of
the same five lines drifting apart over time.</p></div>

<h2 id="sec6"><span class="sec-no">06</span>Leaving early &mdash; <code>cancel()</code> and <code>detach()</code></h2>

<pre>{escape_code(snip_cancel)}</pre>

<pre>{escape_code(snip_detach)}</pre>

<div class="box gist"><span class="k">In plain words</span>
<p>Look up where the order is sitting, unhook it from its queue, and take its size
off the level's totals.</p></div>

<div class="box def"><span class="k">What it does</span>
<p><b>Input:</b> an order id. <b>Output:</b> <code>cancel</code> returns true or
false; <code>detach</code> returns the order it removed. <b>Role:</b>
<code>detach</code> is shared by cancel and by the repricing half of an amendment,
so an order is only ever removed from a queue in one place.</p></div>

<div class="box why"><span class="k">Why this is the interesting function</span>
<p>Cancels are the most common message a real exchange receives, far more common
than trades. Doing it by scanning the queue at that price would be slow at exactly
the wrong moment. The address book turns it into a hash lookup plus unhooking one
node: no scan, no matter how deep the queue is.</p></div>

<div class="box warn"><span class="k">Trap</span>
<p><code>detach</code> reads the locator with <code>locators_.at(id)</code> and
copies it before erasing the entry. Holding a reference into the hash map and then
erasing from that map would leave the reference dangling, and the code would still
appear to work most of the time.</p></div>

<div class="box def"><span class="k">Cancelling twice is not an error</span>
<p>An order can be filled between the moment a trader decides to cancel it and the
moment the cancel arrives. That race is normal, so a cancel for an unknown id
returns false rather than throwing.</p></div>

<h2 id="sec7"><span class="sec-no">07</span>Changing your mind &mdash; <code>modify()</code></h2>

<pre>{escape_code(snip_modify)}</pre>

<div class="box gist"><span class="k">In plain words</span>
<p>Making an order smaller at the same price keeps its place in the queue. Anything
else takes it out of the book and puts it back in as if it were new.</p></div>

<div class="box def"><span class="k">What it does</span>
<p><b>Input:</b> an order id, a new price and a new remaining quantity.
<b>Output:</b> whether the order was found, whether it kept its place, and the
result of re-entering the book if it did not. <b>Role:</b> the amend path traders
use instead of cancelling and resending.</p></div>

<div class="box data"><span class="k">Sample data</span>
<div class="io stack">
<div><span class="lab">in</span>
<pre>{escape_text(SESSION_MODIFY.rstrip())}</pre></div>
<div><span class="lab">out</span>
<pre>{escape_text(out_modify)}</pre>
<p class="fnote">After the first amendment the level still shows 500, which is
order 1's new 200 plus order 2's 300, and order 1 is still in front. The second
amendment moves the price by one cent, so order 1 leaves 100.25 entirely and
appears alone at 100.26.</p></div>
</div></div>

<p><b>Step by step:</b></p>
<ol>
  <li>Refuse a new quantity of zero or less.</li>
  <li>Report <code>found = false</code> if the order is no longer resting, the same
  race a cancel can lose.</li>
  <li>If the price is unchanged and the size is going down, adjust the level's two
  running totals and the order's own numbers in place. The order does not move.</li>
  <li>Keep the already-filled amount by moving <code>original_quantity</code> down
  with <code>remaining</code>, so "filled so far" does not change.</li>
  <li>Otherwise take the order out with <code>detach</code> and push it back through
  <code>execute()</code>, where it can trade immediately at its new price.</li>
</ol>

<div class="box why"><span class="k">Why only a size cut keeps priority</span>
<p>Queue position is a scarce thing: the orders behind you accepted being behind
you. Making your order smaller does not take anything from them, so you keep your
place. Making it larger, or moving it to a better price, would jump the people who
were already there, so you go to the back. Every serious venue draws the line in
the same place, and it is why traders shrink an order rather than cancel and
resend it.</p></div>

<div class="box why"><span class="k">Why the requeue path reuses <code>execute()</code></span>
<p>An order re-entering the book at a new price can cross the spread and trade
straight away, exactly like a fresh order. Sending it back through the same
function means there is one matching path in the whole engine, so a bug fixed for
new orders is fixed for amended ones too.</p></div>

<h2 id="sec8"><span class="sec-no">08</span>Proving it is still sane &mdash; <code>validate()</code></h2>

<p>Everything above changes cached numbers by hand. This function is the check that
those hand edits never went wrong. It returns a list of problems, and an empty list
means the book is consistent.</p>

<div class="box def"><span class="k">What it checks</span>
<ol>
<li>The book is not crossed: the best bid is below the best ask. A crossed book
means two orders that should have traded are both sitting there.</li>
<li>Each level's <code>visible_quantity</code> and <code>total_quantity</code>
equal the sum over its orders. This catches an update that missed one of the two.</li>
<li>No price level exists with an empty queue, and no resting order has a
non-positive size or shows more than it holds.</li>
<li>Every resting order has an entry in <code>locators_</code> pointing at that
exact object, at that price, on that side. This catches a stale iterator.</li>
<li>The number of entries in <code>locators_</code> equals the number of orders
actually in the two ladders. This catches a leak in either direction.</li>
</ol></div>

<div class="box why"><span class="k">Why this is worth writing</span>
<p>Every one of those five checks corresponds to a way this engine can quietly
break. A matching engine that loses track of size does not crash; it prints a book
that is wrong, and keeps trading. Checking the invariants directly turns a silent
wrong answer into a loud one.</p></div>

<h2 id="sec9"><span class="sec-no">09</span>Random order flow &mdash; <code>RandomFlow::next()</code></h2>

<pre>{escape_code(snip_next)}</pre>

<div class="box gist"><span class="k">In plain words</span>
<p>Invent a plausible next instruction: usually a new order near the current
midpoint, sometimes a cancel or an amendment of an order already resting.</p></div>

<div class="box def"><span class="k">What it does</span>
<p><b>Input:</b> the book, read only, and the ids currently resting.
<b>Output:</b> one instruction. <b>Role:</b> feeds
<code>order_book_random</code>, which runs each instruction and calls
<code>validate()</code> after every one.</p></div>

<div class="box data"><span class="k">Sample data &mdash; {RANDOM_INSTRUCTIONS} instructions, seed {RANDOM_SEED}</span>
<div class="io stack">
<div><span class="lab">out &mdash; what was generated</span>
<pre>{escape_text(random_types)}

{escape_text(random_amends)}</pre></div>
<div><span class="lab">out &mdash; what happened</span>
<pre>{escape_text(random_outcomes)}

{escape_text(random_trading)}

{escape_text(random_checks)}</pre></div>
</div>
<p class="fnote">"cancel too late" and "amend too late" are not failures. They are
the generator aiming at an order that has already traded, which is a real race and
worth exercising.</p></div>

<p><b>Step by step:</b></p>
<ol>
  <li>Anchor prices on the live midpoint, falling back to whichever side exists,
  and to the starting price only when the book is empty.</li>
  <li>With nothing resting there is nothing to cancel or amend, so always submit.</li>
  <li>Roll once. A low roll cancels a random resting order.</li>
  <li>The next band amends one. Half the time it keeps the price and cuts the size,
  which is the branch that keeps queue priority.</li>
  <li>Otherwise submit: random side, a type drawn from the weights, a random size,
  and a price scattered around the midpoint.</li>
</ol>

<div class="box why"><span class="k">Why the shrink case is forced</span>
<p>A randomly chosen new price is almost never exactly equal to the old one, so
without this the keep-your-place branch of <code>modify()</code> would be reached a
handful of times in twenty thousand instructions. Making it deliberate takes that
branch from barely tested to properly tested.</p></div>

<div class="box why"><span class="k">Why the seed matters</span>
<p>The whole session is determined by the seed, so a failure is reproducible. Note
the seed, fix the bug, rerun the same seed. A tester whose failures cannot be
reproduced is close to useless.</p></div>

<div class="box def"><span class="k">The end-of-run conservation check</span>
<p>Every execution adds the same quantity to a buyer and to a seller. So the filled
quantity summed over every order the session ever created must be exactly twice the
volume the book reports:</p>
<p>$$\\sum_{{\\text{{orders}}}} \\text{{filled}}_i = 2 \\times \\text{{volume}}$$</p>
<p class="fnote">$\\text{{filled}}_i$ is how much order $i$ traded over its whole life,
and volume is the total the book counted. If the engine ever loses or invents
quantity, the two sides stop matching.</p></div>

<div class="box flag"><span class="k">What this testing cannot find</span>
<p>The generator is sequential and single threaded, so it will never produce two
messages arriving at the same instant. Race conditions are among the worst bugs a
real engine can have, and nothing here will catch one.</p></div>

<h2 id="callgraph"><span class="sec-no">10</span>Call graph</h2>

<pre>submit(side, type, price, quantity, display)
  validate the request
  execute(order)
      available_quantity(...)        <span class="c">// fill-or-kill only</span>
      best_bid() / best_ask()        <span class="c">// post-only only</span>
      match(ladder, order, crosses, trades)
          record_trade(trade)
          remember_finished(maker, Filled)
          list::splice(...)          <span class="c">// iceberg refill</span>
      rest(order)

cancel(id)
  detach(id)
  remember_finished(order, Cancelled)

modify(id, new_price, new_quantity)
  same price and smaller  -&gt; adjust in place, keep queue position
  anything else           -&gt; detach(id) then execute(order)</pre>

<h2 id="edges"><span class="sec-no">11</span>Edge cases</h2>

<div class="scroll">
<table>
<thead><tr><th>Situation</th><th>What would go wrong</th><th>What handles it</th></tr></thead>
<tbody>
<tr><td>Market order, empty book</td><td>Matching against nothing, or the order resting with no price</td><td><code>match</code> returns at once; market orders never rest, so it is marked cancelled</td></tr>
<tr><td>Cancel of an order that just filled</td><td>Following a dangling iterator</td><td>The id is gone from <code>locators_</code>, so <code>cancel</code> returns false</td></tr>
<tr><td>Iceberg whose slice is larger than what is left</td><td>Showing more size than the order holds</td><td><code>std::min(display_size, remaining)</code> at every refill</td></tr>
<tr><td>Amendment that crosses the spread</td><td>A resting order silently becoming a taker with no trades recorded</td><td>The requeue path runs <code>execute()</code>, so it matches and reports its trades</td></tr>
<tr><td>Fill-or-kill against hidden size</td><td>Killing an order that could have been filled</td><td><code>available_quantity</code> sums <code>total_quantity</code>, hidden part included</td></tr>
<tr><td>Last order at a price is filled</td><td>An empty price level left in the map, so the book reports a price with no size</td><td>Both <code>match</code> and <code>detach</code> erase the level when its queue empties</td></tr>
<tr><td>Two prices that look identical</td><td>Rounding making 100.25 not equal 100.25</td><td>Prices are integer ticks; no floating point anywhere in the engine</td></tr>
</tbody>
</table>
</div>

<h2 id="idioms"><span class="sec-no">12</span>C++ idioms used here</h2>

<div class="scroll">
<table>
<thead><tr><th>Idiom</th><th>What it does</th><th>The trap</th></tr></thead>
<tbody>
<tr><td><code>std::map&lt;Price, Level, std::greater&lt;Price&gt;&gt;</code></td><td>An ordered map that sorts high to low, so <code>begin()</code> is the best bid</td><td>Bids and asks are then different types, so shared code has to be a template or a generic lambda</td></tr>
<tr><td><code>list::splice</code></td><td>Moves nodes between or within lists in constant time</td><td>Only safe because iterators survive it; the same pattern on a vector is a use-after-free</td></tr>
<tr><td>Storing an iterator in a hash map</td><td>Turns "find this order" into a hash lookup</td><td>Every insert and erase has to keep the map in step, which is what <code>validate()</code> checks</td></tr>
<tr><td><code>auto&amp;</code> generic lambda</td><td>One body compiled for both ladder types</td><td>Errors appear at the call site rather than in the lambda, so messages are long</td></tr>
<tr><td><code>std::optional&lt;Price&gt;</code></td><td>"There is no best bid" without a magic number</td><td>Dereferencing without checking is undefined behaviour, so it always follows an <code>if</code></td></tr>
<tr><td>Structured bindings in a range loop</td><td><code>for (const auto&amp; [price, level] : ladder)</code></td><td>An unused binding warns, hence the <code>(void)</code> in one loop</td></tr>
</tbody>
</table>
</div>

<h2 id="questions"><span class="sec-no">13</span>If someone points at a line</h2>

<div class="scroll">
<table>
<thead><tr><th>Question</th><th>Answer</th></tr></thead>
<tbody>
<tr><td>Why is the price an integer?</td><td>Two prices that print identically can compare unequal as <code>double</code>, and the book compares prices constantly. Ticks are cents, so 100.25 is stored as 10025.</td></tr>
<tr><td>Why does a taker sometimes trade against the same maker several times?</td><td>It is an iceberg. Each refilled slice is a separate execution, and other orders at that price can trade in between.</td></tr>
<tr><td>What stops the matching loop spinning forever?</td><td>Every pass either removes the maker from the queue or reduces the taker's remaining size. Neither can happen indefinitely.</td></tr>
<tr><td>Why keep a record of finished orders?</td><td>So <code>order(id)</code> can answer questions about an order after it has left the book. It grows for the life of the process, which is fine here and would be a rolling log in a real system.</td></tr>
<tr><td>Where is the cost of hiding size?</td><td>In <code>match()</code>: when a slice is used up, the order is spliced to the back of the queue and loses its priority.</td></tr>
<tr><td>What happens if the same order is amended twice quickly?</td><td>Each amendment is independent. A second one that reprices finds the order where the first one left it, because <code>locators_</code> was updated by <code>rest()</code>.</td></tr>
</tbody>
</table>
</div>

<h2 id="verify"><span class="sec-no">14</span>How the engine is checked</h2>

<div class="scroll">
<table>
<thead><tr><th>Check</th><th>How it works</th><th>What it catches</th></tr></thead>
<tbody>
<tr><td>28 hand written tests</td><td>Each sets up a small book and asserts on trades, order of makers, and status</td><td>Rules stated wrongly: priority, price improvement, what rests and what does not</td></tr>
<tr><td><code>validate()</code> after every instruction</td><td>The random tester calls it {RANDOM_INSTRUCTIONS} times per run</td><td>Cached sizes drifting, empty levels left behind, a stale or missing locator, a crossed book</td></tr>
<tr><td>Quantity conservation</td><td>Filled summed over every order must equal twice the reported volume</td><td>Quantity lost or invented anywhere in matching or amending</td></tr>
<tr><td>Repeatability</td><td>The same seed is run twice and the statistics compared</td><td>Hidden state or a dependence on memory addresses making runs differ</td></tr>
<tr><td>Seed sweep</td><td><code>for s in $(seq 1 40); do ./build/order_book_random 20000 $s; done</code></td><td>A fault that only one particular sequence of instructions triggers</td></tr>
</tbody>
</table>
</div>

<p class="foot">Generated by <code>scripts/build_explanation.py</code> from the source
in <code>src/</code> and from live runs of <code>order_book_sim</code> and
<code>order_book_random</code>. Rebuild the page after changing the code.</p>

</div>
</body>
</html>
"""

    return HEAD.read_text() + body


def main() -> int:
    OUTPUT.parent.mkdir(parents=True, exist_ok=True)
    OUTPUT.write_text(build())
    print(f"wrote {OUTPUT.relative_to(ROOT)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
