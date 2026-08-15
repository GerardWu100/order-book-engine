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
    """Pull one titled block out of the random tester's output.

    The speed line is dropped: it is a timing measurement, so keeping it would
    change the page on every build for no new information.
    """
    blocks = output.split("\n\n")
    for block in blocks:
        if block.strip().startswith(heading):
            kept = [line for line in block.rstrip().splitlines() if "instructions/sec" not in line]
            return "\n".join(kept)
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

    body = rf"""
<div class="wrap">

<p class="eyebrow"><span class="dot"></span>ORDER BOOK ENGINE &middot; C++ &middot; code guide</p>
<h{RANDOM_SEED}>How this order book works</h{RANDOM_SEED}>
<p class="lede">A matching engine is a queue with two rules: price first, then
arrival time. This page follows an order through the code from arrival to exit.</p>

<div class="box def">
  <span class="k">How to use this</span>
  <p>Sections follow an order through the engine. The build script copies each
  snippet from <code>src/</code>, and every number in a <b>Sample data</b> box
  came from a live run. Coloured boxes call out design choices and traps.</p>
</div>

<div class="box why">
  <span class="k">The whole engine in plain English</span>
  <p>Buyers and sellers send orders. The engine sorts unfilled orders by best
  price, then by age. A new order checks the best order on the other side. If the
  prices overlap, they trade at the older order's price. The new order's remainder
  either joins the queue or is discarded, depending on its type.</p>
  <p>The difficult parts are cancelling an order in the middle of a queue without
  searching for it, and keeping each price level's cached quantities correct.</p>
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
<p class="fnote">Boxes are functions, the cylinder is the stored book, and arrows
show the flow. <code>submit()</code>, <code>cancel()</code> and
<code>modify()</code> are the public entry points; the rest is internal.</p>

<div class="toc">
  <div class="rk">Contents</div>
  <ol>
    <li><a href="#sec{RANDOM_SEED}">Where orders live &mdash; the three structures</a></li>
    <li><a href="#sec2">The front door &mdash; <code>submit()</code></a></li>
    <li><a href="#sec3">Check, match, wait &mdash; <code>execute()</code></a></li>
    <li><a href="#sec4">The matching loop &mdash; <code>match()</code></a></li>
    <li><a href="#sec5">Joining the queue &mdash; <code>rest()</code></a></li>
    <li><a href="#sec6">Leaving early &mdash; <code>cancel()</code> and <code>detach()</code></a></li>
    <li><a href="#sec7">Changing an order &mdash; <code>modify()</code></a></li>
    <li><a href="#sec8">Checking the book &mdash; <code>validate()</code></a></li>
    <li><a href="#sec9">Random order flow &mdash; <code>RandomFlow::next()</code></a></li>
  </ol>
</div>

<h2 id="sec{RANDOM_SEED}"><span class="sec-no">0{RANDOM_SEED}</span>Where orders live &mdash; three structures</h2>

<p>These three choices determine how the rest of the engine behaves.</p>

<pre>{escape_code(snip_level)}</pre>

<pre>{escape_code(snip_ladders)}</pre>

<div class="box gist"><span class="k">The idea</span>
<p>The book is a sorted list of prices. Each price holds an order queue. A
separate lookup records each order's exact position, so a cancel needs no search.</p></div>

<div class="box def"><span class="k">What each one does</span>
<p><b><code>bids_</code> and <code>asks_</code>:</b> ordered maps from price to
its queue. Bids sort high to low and asks low to high, so <code>begin()</code>
is always the best price.<br>
<b><code>PriceLevel::orders</code>:</b> a <code>std::list</code>, or doubly linked
list. The front is oldest; the back is newest.<br>
<b><code>locators_</code>:</b> a hash map from order ID to its side, price, and
exact position in the queue.</p></div>

<div class="box why"><span class="k">Why a map and not a heap</span>
<p>A binary heap gives cheap access to the best price, but it is poor at removing
an order at an arbitrary price. An ordered map keeps the best price at
<code>begin()</code> and can also find or erase any price by key.</p></div>

<div class="box why"><span class="k">Why a linked list and not a vector</span>
<p><code>locators_</code> stores an iterator into the queue. Removing from the
middle of a vector shifts later elements and invalidates their positions. A linked
list changes only its two neighbours, so the other iterators stay valid.</p></div>

<div class="box def"><span class="k">Two sizes per level, not one</span>
<p><code>visible_quantity</code> is what the market can see.
<code>total_quantity</code> also includes hidden iceberg quantity. Without an
iceberg the two are equal. Running totals let the engine read a level's depth
without walking its queue.</p></div>

<div class="box warn"><span class="k">Trap</span>
<p>These totals are easy to desynchronise. Every size change must update both, or
the book will report quantity that does not exist. Section 08's
<code>validate()</code> check catches this, and the random tester runs it after
every instruction.</p></div>

<h2 id="sec2"><span class="sec-no">02</span>The front door &mdash; <code>submit()</code></h2>

<pre>{escape_code(snip_submit)}</pre>

<div class="box gist"><span class="k">The idea</span>
<p>Check the request, assign an order number, and pass it to the matching path.</p></div>

<div class="box def"><span class="k">Purpose</span>
<p><b>Input:</b> side, type, price, quantity, and an iceberg display size.
<b>Output:</b> a <code>SubmitResult</code> with the order ID, trades, final status,
and whether any quantity is still waiting. <b>Role:</b> the only entry point for
a new order.</p></div>

<div class="box data"><span class="k">Sample data</span>
<div class="io stack">
<div><span class="lab">in &mdash; four instructions</span>
<pre>{escape_text(SESSION_MATCH.rstrip())}</pre></div>
<div><span class="lab">out &mdash; what the engine printed</span>
<pre>{escape_text(out_match)}</pre>
<p class="fnote">The buyer would pay up to {RANDOM_SEED}00.40, but trades at the sellers' prices:
{RANDOM_SEED}00.30 for the first 200 and {RANDOM_SEED}00.35 for the next 200. The buyer paid 40{RANDOM_SEED}30 rather
than 40{RANDOM_SEED}60, the cost of 400 at its limit.</p></div>
</div></div>

<p><b>What happens:</b></p>
<ol>
  <li>Reject a non-positive quantity with an exception.</li>
  <li>Reject an iceberg with no display size.</li>
  <li>Create an <code>Order</code> with a new ID and the requested quantities.</li>
  <li>Force a market order's price to <code>kNoPrice</code>.</li>
  <li>Pass the order to <code>execute()</code>.</li>
</ol>

<div class="box why"><span class="k">Why three quantity fields</span>
<p><code>original_quantity</code> is the starting size, so filled quantity is
<code>original - remaining</code>. <code>remaining</code> is what can still trade;
<code>visible</code> is what the market can see. Deriving filled quantity avoids a
fourth number that could get out of sync.</p></div>

<h2 id="sec3"><span class="sec-no">03</span>Checks, match, rest &mdash; <code>execute()</code></h2>

<pre>{escape_code(snip_execute)}</pre>

<div class="box gist"><span class="k">The idea</span>
<p>Post-only and fill-or-kill get checked before trading. Everything else matches
first; the remainder either waits in the book or is discarded.</p></div>

<div class="box def"><span class="k">Purpose</span>
<p><b>Input:</b> a complete new or amended order. <b>Output:</b> its trades, final
status, and whether it is still waiting. <b>Role:</b> the one path shared by new
and amended orders.</p></div>

<div class="box data"><span class="k">Sample data &mdash; post-only</span>
<div class="io stack">
<div><span class="lab">in</span>
<pre>{escape_text(SESSION_POSTONLY.rstrip())}</pre></div>
<div><span class="lab">out</span>
<pre>{escape_text(out_postonly)}</pre>
<p class="fnote">Order 2 would have traded immediately, so it was refused. Order 3
was one cent lower, so it waited and became the best bid.</p></div>
</div></div>

<div class="box data"><span class="k">Sample data &mdash; fill-or-kill</span>
<div class="io stack">
<div><span class="lab">in</span>
<pre>{escape_text(SESSION_FOK.rstrip())}</pre></div>
<div><span class="lab">out</span>
<pre>{escape_text(out_fok)}</pre>
<p class="fnote">Only {RANDOM_SEED}00 was available at or below {RANDOM_SEED}00.35. The 900 at {RANDOM_SEED}00.50 was too
expensive, so the order was killed before trading and both sellers stayed put.</p></div>
</div></div>

<p><b>What happens:</b></p>
<ol>
  <li>Stamp an arrival number. An amended order gets a new one and loses its place.</li>
  <li>Reject post-only if the best price on the other side would let it trade.</li>
  <li>Check fill-or-kill before trading; if the full size is unavailable, change nothing.</li>
  <li>Match against prices that cross the incoming order's limit.</li>
  <li>Let limit, iceberg, and post-only remainders wait; discard the others.</li>
  <li>Set the final status and record orders that have left the book.</li>
</ol>

<div class="box why"><span class="k">Why fill-or-kill is checked first, not attempted</span>
<p>Trading first and undoing a partial fill would mean restoring several orders to
their old queue positions. Counting available quantity first is one pass through
the relevant price levels and cannot leave a half-reversed trade.</p></div>

<div class="box def"><span class="k">Hidden size counts as real liquidity</span>
<p>The check calls <code>available_quantity</code>, which sums each level's
<code>total_quantity</code>, including hidden iceberg quantity. Hidden quantity can
trade, so ignoring it would reject orders that could be filled.</p></div>

<div class="box flag"><span class="k">The choice, stated honestly</span>
<p>Some venues count only displayed size, which makes FOK orders fail more often
but keeps hidden size hidden. This engine counts all executable size. Change the
argument passed to <code>available_quantity</code> to choose the other rule.</p></div>

<h2 id="sec4"><span class="sec-no">04</span>The matching loop &mdash; <code>match()</code></h2>

<pre>{escape_code(snip_match)}</pre>

<div class="box gist"><span class="k">The idea</span>
<p>Take from the best price on the other side, oldest order first, until the
incoming order is filled or the next price no longer qualifies.</p></div>

<div class="box def"><span class="k">Purpose</span>
<p><b>Input:</b> one side of the book, the incoming order, and a test for prices it
can trade at. <b>Output:</b> trades appended to a list and a reduced
<code>incoming.remaining</code>. <b>Role:</b> the only function that creates trades.</p></div>

<div class="box data"><span class="k">Sample data &mdash; an iceberg refilling</span>
<div class="io stack">
<div><span class="lab">in</span>
<pre>{escape_text(SESSION_ICEBERG.rstrip())}</pre></div>
<div><span class="lab">out</span>
<pre>{escape_text(out_iceberg)}</pre>
<p class="fnote">Order {RANDOM_SEED} shows {RANDOM_SEED}00 of its {RANDOM_SEED}000 total, so the level shows 200, not
{RANDOM_SEED}{RANDOM_SEED}00. The 250-unit buy traded {RANDOM_SEED}00 from order {RANDOM_SEED}, {RANDOM_SEED}00 from order 2, then 50 from
order {RANDOM_SEED}'s next slice. When that first slice ran out, order {RANDOM_SEED} moved behind order 2.</p></div>
</div></div>

<p><b>What happens:</b></p>
<ol>
  <li>Look at the best price on the other side and stop if it no longer qualifies.</li>
  <li>Trade with the oldest order there for the smaller of the two visible quantities.</li>
  <li>Reduce the incoming order, maker order, and both level totals by the fill.</li>
  <li>Record the trade at the maker's price.</li>
  <li>Remove a fully filled maker from the queue.</li>
  <li>Refresh an exhausted iceberg slice and move that order to the back.</li>
  <li>Erase a price level when its queue becomes empty.</li>
</ol>

<div class="box why"><span class="k">Why <code>splice</code> and not erase-and-push</span>
<p><code>splice</code> relinks the existing node. Nothing is copied, and the
iterator in <code>locators_</code> still points to the same order. Erasing and
reinserting would leave that iterator pointing at freed memory.</p></div>

<div class="box why"><span class="k">Why the trade prints at the maker's price</span>
<p>The older order named its price first. The incoming order is willing to pay up
to its limit, so any gap between the two is price improvement for the incoming
order. In the sample output, the incoming order is the <i>taker</i> and the older
one is the <i>maker</i>.</p></div>

<div class="box warn"><span class="k">Trap</span>
<p>The loop references <code>level.orders.front()</code> and, for an iceberg, moves
that node to the back. The reference stays valid because <code>splice</code> moves
nodes instead of copying them. The loop also always makes progress: it either
removes the maker or reduces the incoming quantity.</p></div>

<h2 id="sec5"><span class="sec-no">05</span>Joining the queue &mdash; <code>rest()</code></h2>

<pre>{escape_code(snip_rest)}</pre>

<div class="box gist"><span class="k">The idea</span>
<p>Put the leftover at the back of its price queue and record its exact position.</p></div>

<div class="box def"><span class="k">Purpose</span>
<p><b>Input:</b> an order with quantity left. <b>Output:</b> no return value; the
book and locator map are updated. <b>Role:</b> the only function that adds an
order to a queue.</p></div>

<p><b>What happens:</b></p>
<ol>
  <li>Choose the displayed size: one slice for an iceberg, the full size otherwise.</li>
  <li>Find the price level, creating it if necessary.</li>
  <li>Add the order to the back of the queue.</li>
  <li>Increase both level totals.</li>
  <li>Record the side, price, and exact position in <code>locators_</code>.</li>
</ol>

<div class="box why"><span class="k">Why the same lambda for both sides</span>
<p><code>bids_</code> and <code>asks_</code> sort in opposite directions, so they are
different types. A generic lambda taking <code>auto&amp;</code> lets one block of
code work with both and prevents the two versions from drifting apart.</p></div>

<h2 id="sec6"><span class="sec-no">06</span>Leaving early &mdash; <code>cancel()</code> and <code>detach()</code></h2>

<pre>{escape_code(snip_cancel)}</pre>

<pre>{escape_code(snip_detach)}</pre>

<div class="box gist"><span class="k">The idea</span>
<p>Look up the order, remove it from its queue, and subtract its size from the
level totals.</p></div>

<div class="box def"><span class="k">Purpose</span>
<p><b>Input:</b> an order ID. <b>Output:</b> <code>cancel</code> returns true or
false; <code>detach</code> returns the removed order. <b>Role:</b>
<code>detach</code> is shared by cancellation and repricing, so removal logic lives
in one place.</p></div>

<div class="box why"><span class="k">Why this matters</span>
<p>Scanning a price queue makes a deep cancel slow. The locator map turns it into a
hash lookup followed by removing one node, no matter where the order sits.</p></div>

<div class="box warn"><span class="k">Trap</span>
<p><code>detach</code> copies the locator before erasing its map entry. A reference
into the hash map would become invalid at the erase, even if the bug appeared to
work most of the time.</p></div>

<div class="box def"><span class="k">Cancelling twice is not an error</span>
<p>An order can fill before its cancel arrives. That is normal, so an unknown ID
returns false rather than throwing.</p></div>

<h2 id="sec7"><span class="sec-no">07</span>Changing an order &mdash; <code>modify()</code></h2>

<pre>{escape_code(snip_modify)}</pre>

<div class="box gist"><span class="k">The idea</span>
<p>Making an order smaller at the same price keeps its place. Any other change
removes it and re-enters it as a new order.</p></div>

<div class="box def"><span class="k">Purpose</span>
<p><b>Input:</b> an order ID, new price, and new remaining quantity.
<b>Output:</b> whether it was found, whether it kept its place, and the result if
it re-enters the book. <b>Role:</b> the path for changing a waiting order.</p></div>

<div class="box data"><span class="k">Sample data</span>
<div class="io stack">
<div><span class="lab">in</span>
<pre>{escape_text(SESSION_MODIFY.rstrip())}</pre></div>
<div><span class="lab">out</span>
<pre>{escape_text(out_modify)}</pre>
<p class="fnote">After the first change the level still shows 500: order {RANDOM_SEED} has 200
and order 2 has 300, with order {RANDOM_SEED} still first. The one-cent price change moves
order {RANDOM_SEED} to {RANDOM_SEED}00.26, where it is the only order.</p></div>
</div></div>

<p><b>What happens:</b></p>
<ol>
  <li>Reject a non-positive new quantity.</li>
  <li>Return <code>found = false</code> if the order has already left the book.</li>
  <li>For a same-price reduction, update the two totals and order in place.</li>
  <li>Adjust <code>original_quantity</code> so filled quantity stays unchanged.</li>
  <li>Otherwise remove the order with <code>detach</code> and pass it through
  <code>execute()</code>, where it can trade at its new price.</li>
</ol>

<div class="box why"><span class="k">Why only a size cut keeps priority</span>
<p>A smaller order does not jump the orders behind it, so it keeps its place. A
larger order or a better price would jump them, so the changed order goes to the
back. This is why reducing size is different from cancelling and resending.</p></div>

<div class="box why"><span class="k">Why the requeue path reuses <code>execute()</code></span>
<p>An order at a new price may cross the spread and trade immediately. Sending it
through <code>execute()</code> gives new and changed orders the same matching rules.</p></div>

<h2 id="sec8"><span class="sec-no">08</span>Checking the book &mdash; <code>validate()</code></h2>

<p>The functions above update cached numbers as orders change. This function checks
those updates. It returns a list of problems; an empty list means the book is
consistent.</p>

<div class="box def"><span class="k">What it checks</span>
<ol>
<li>The book is not crossed: the best bid is below the best ask.</li>
<li>Each level's <code>visible_quantity</code> and <code>total_quantity</code> equal
the sum of its orders.</li>
<li>No price level is empty, and no waiting order has a non-positive size or shows
more quantity than it holds.</li>
<li>Every waiting order has a <code>locators_</code> entry pointing to the right
object, price, and side.</li>
<li>The locator count equals the number of orders in the two ladders.</li>
</ol></div>

<div class="box why"><span class="k">Why this is worth writing</span>
<p>These checks catch failures that would otherwise produce a wrong book without
crashing. The engine reports the problem instead of continuing with bad data.</p></div>

<h2 id="sec9"><span class="sec-no">09</span>Random order flow &mdash; <code>RandomFlow::next()</code></h2>

<pre>{escape_code(snip_next)}</pre>

<div class="box gist"><span class="k">The idea</span>
<p>Generate the next plausible instruction: usually a new order near the current
midpoint, sometimes a cancel or change to an order already waiting.</p></div>

<div class="box def"><span class="k">Purpose</span>
<p><b>Input:</b> the read-only book and IDs currently waiting.
<b>Output:</b> one instruction. <b>Role:</b> feeds <code>order_book_random</code>,
which calls <code>validate()</code> after every instruction.</p></div>

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
<p class="fnote">"cancel too late" and "amend too late" are expected. The generator
selected an order that had already traded, a normal race worth testing.</p></div>

<p><b>What happens:</b></p>
<ol>
  <li>Base prices on the live midpoint, then whichever side exists, then the
  starting price if the book is empty.</li>
  <li>If nothing is waiting, submit because there is nothing to cancel or change.</li>
  <li>Use a random draw to choose a cancel or change when possible.</li>
  <li>For half of the changes, keep the price and reduce size to test queue priority.</li>
  <li>Otherwise submit a random side, type, size, and price near the midpoint.</li>
</ol>

<div class="box why"><span class="k">Why the shrink case is forced</span>
<p>A random new price is rarely exactly the old price. Deliberately shrinking at
the same price gives the queue-preserving branch of <code>modify()</code> reliable
coverage.</p></div>

<div class="box why"><span class="k">Why the seed matters</span>
<p>The seed determines the whole session. Record it, fix the bug, and rerun the
same sequence. Reproducible failures are what make this tester useful.</p></div>

<div class="box def"><span class="k">The end-of-run conservation check</span>
<p>Every execution adds the same quantity to one buyer and one seller. Therefore,
filled quantity across all orders must be twice the volume reported by the book:</p>
<p>$$\sum_{{\text{{orders}}}} \text{{filled}}_i = 2 \times \text{{volume}}$$</p>
<p class="fnote">$\text{{filled}}_i$ is the lifetime traded quantity for order $i$;
volume is the book's total. If quantity is lost or invented, the two sides differ.</p></div>

<div class="box flag"><span class="k">What this testing cannot find</span>
<p>The generator is sequential and single-threaded, so it never creates two
messages arriving at once. It cannot find race conditions between threads.</p></div>

<h2 id="callgraph"><span class="sec-no">{RANDOM_SEED}0</span>Call flow</h2>

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

<h2 id="edges"><span class="sec-no">{RANDOM_SEED}{RANDOM_SEED}</span>Cases worth checking</h2>

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
<tr><td>Two prices that look identical</td><td>Rounding making {RANDOM_SEED}00.25 not equal {RANDOM_SEED}00.25</td><td>Prices are integer ticks; no floating point anywhere in the engine</td></tr>
</tbody>
</table>
</div>

<h2 id="idioms"><span class="sec-no">{RANDOM_SEED}2</span>C++ techniques used here</h2>

<div class="scroll">
<table>
<thead><tr><th>Technique</th><th>Purpose</th><th>The trap</th></tr></thead>
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

<h2 id="questions"><span class="sec-no">{RANDOM_SEED}3</span>Questions the code should answer</h2>

<div class="scroll">
<table>
<thead><tr><th>Question</th><th>Answer</th></tr></thead>
<tbody>
<tr><td>Why is the price an integer?</td><td>Two prices that print identically can compare unequal as <code>double</code>, and the book compares prices constantly. Ticks are cents, so {RANDOM_SEED}00.25 is stored as {RANDOM_SEED}0025.</td></tr>
<tr><td>Why does a taker sometimes trade against the same maker several times?</td><td>It is an iceberg. Each refilled slice is a separate execution, and other orders at that price can trade in between.</td></tr>
<tr><td>What stops the matching loop spinning forever?</td><td>Every pass either removes the maker from the queue or reduces the taker's remaining size. Neither can happen indefinitely.</td></tr>
<tr><td>Why keep a record of finished orders?</td><td>So <code>order(id)</code> can answer questions about an order after it has left the book. It grows for the life of the process, which is fine here and would be a rolling log in a real system.</td></tr>
<tr><td>Where is the cost of hiding size?</td><td>In <code>match()</code>: when a slice is used up, the order is spliced to the back of the queue and loses its priority.</td></tr>
<tr><td>What happens if the same order is amended twice quickly?</td><td>Each amendment is independent. A second one that reprices finds the order where the first one left it, because <code>locators_</code> was updated by <code>rest()</code>.</td></tr>
</tbody>
</table>
</div>

<h2 id="verify"><span class="sec-no">{RANDOM_SEED}4</span>How the engine is tested</h2>

<div class="scroll">
<table>
<thead><tr><th>Check</th><th>How it works</th><th>What it catches</th></tr></thead>
<tbody>
<tr><td>28 hand written tests</td><td>Each sets up a small book and asserts on trades, order of makers, and status</td><td>Rules stated wrongly: priority, price improvement, what rests and what does not</td></tr>
<tr><td><code>validate()</code> after every instruction</td><td>The random tester calls it {RANDOM_INSTRUCTIONS} times per run</td><td>Cached sizes drifting, empty levels left behind, a stale or missing locator, a crossed book</td></tr>
<tr><td>Quantity conservation</td><td>Filled summed over every order must equal twice the reported volume</td><td>Quantity lost or invented anywhere in matching or amending</td></tr>
<tr><td>Repeatability</td><td>The same seed is run twice and the statistics compared</td><td>Hidden state or a dependence on memory addresses making runs differ</td></tr>
<tr><td>Seed sweep</td><td><code>for s in $(seq {RANDOM_SEED} 40); do ./build/order_book_random {RANDOM_INSTRUCTIONS} $s; done</code></td><td>A fault that only one particular sequence of instructions triggers</td></tr>
</tbody>
</table>
</div>

<p class="foot">Generated by <code>scripts/build_explanation.py</code> from
<code>src/</code> and live runs of <code>order_book_sim</code> and
<code>order_book_random</code>. Rebuild after changing the code.</p>

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
