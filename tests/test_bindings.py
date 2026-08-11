"""Smoke test for the pybind11 bindings.

Run with the built module on the path:
    PYTHONPATH=build python3 tests/test_bindings.py
"""
import matchbook as mb

# --- basic match, price-time priority, execution at maker price ---
e = mb.Engine(1, 10000)
s1 = e.submit_limit(mb.Side.Sell, 101, 5)
s2 = e.submit_limit(mb.Side.Sell, 100, 5)
assert e.has_ask() and e.best_ask() == 100
b = e.submit_limit(mb.Side.Buy, 101, 8)
trades = e.take_trades()
assert [t.maker for t in trades] == [s2, s1], trades
assert [t.price for t in trades] == [100, 101]  # maker's price
assert [t.qty for t in trades] == [5, 3]
assert e.take_trades() == []  # drained
assert e.open_orders() == 1   # s1 remainder rests

# --- cancel / modify / reduce ---
assert e.cancel(s1)
assert not e.cancel(s1)  # already gone
assert e.open_orders() == 0
o = e.submit_limit(mb.Side.Buy, 50, 10)
assert e.reduce(o, 4)          # amend-down keeps priority
assert e.modify(o, 60, 6)      # reprice = cancel-replace
assert e.best_bid() == 60

# --- IOC / FOK ---
e2 = mb.Engine(1, 10000)
e2.submit_limit(mb.Side.Sell, 100, 5)
ioc = e2.submit_limit(mb.Side.Buy, 100, 8, mb.TimeInForce.IOC)
assert sum(t.qty for t in e2.take_trades()) == 5  # filled 5, rest cancelled
assert e2.open_orders() == 0
e2.submit_limit(mb.Side.Sell, 100, 5)
e2.submit_limit(mb.Side.Buy, 100, 8, mb.TimeInForce.FOK)
assert e2.take_trades() == []  # not enough depth: nothing executes
assert e2.open_orders() == 1

# --- band rejection ---
e3 = mb.Engine(10, 20)
e3.submit_limit(mb.Side.Buy, 5, 1)
assert e3.rejects() == 1

# --- post-only ---
e4 = mb.Engine(1, 10000)
e4.submit_limit(mb.Side.Sell, 100, 5)
po = e4.submit_limit(mb.Side.Buy, 100, 5, mb.TimeInForce.PostOnly)
assert e4.take_trades() == []          # would cross: killed, took nothing
assert po in e4.take_cancels()
e4.submit_limit(mb.Side.Buy, 99, 5, mb.TimeInForce.PostOnly)
assert e4.best_bid() == 99             # passive: rests

# --- self-trade prevention ---
e5 = mb.Engine(1, 10000)
mine = e5.submit_limit(mb.Side.Sell, 100, 5, owner=7)
taker = e5.submit_limit(mb.Side.Buy, 100, 8, owner=7)  # default CancelTaker
assert e5.take_trades() == []
assert e5.take_cancels() == [taker]
assert e5.depth_at(mb.Side.Sell, 100) == 5             # maker survives
e5.submit_limit(mb.Side.Buy, 100, 5, owner=7, stp=mb.StpPolicy.CancelMaker)
assert e5.take_cancels() == [mine]                     # maker wiped instead

# --- iceberg + depth snapshot ---
e6 = mb.Engine(1, 10000)
ice = e6.submit_iceberg(mb.Side.Sell, 100, 25, 10)
assert e6.depth_at(mb.Side.Sell, 100) == 10            # clip shows
assert e6.hidden_at(mb.Side.Sell, 100) == 15           # reserve dark
top = e6.top_levels(mb.Side.Sell, 5)
assert len(top) == 1 and top[0].price == 100 and top[0].qty == 10
assert top[0].orders == 1
e6.submit_limit(mb.Side.Buy, 100, 25)
assert [t.qty for t in e6.take_trades()] == [10, 10, 5]  # three clips
assert e6.open_orders() == 0

# --- stops ---
e7 = mb.Engine(1, 10000)
e7.submit_limit(mb.Side.Sell, 100, 5)
e7.submit_limit(mb.Side.Sell, 105, 5)
stop = e7.submit_stop(mb.Side.Buy, 103, 5)
assert e7.pending_stops() == 1
assert e7.stop_depth_at(mb.Side.Buy, 103) == 5
e7.submit_limit(mb.Side.Buy, 100, 5)                   # prints 100: parked
assert e7.pending_stops() == 1
e7.submit_limit(mb.Side.Buy, 105, 5)                   # prints 105: fires
assert e7.pending_stops() == 0
assert e7.last_trade() == 105
sl = e7.submit_stop_limit(mb.Side.Sell, 104, 103, 3)   # 105 > 104: parked
assert e7.pending_stops() == 1
assert e7.cancel(sl)                                   # cancellable while parked

# --- auction: halt, uncross at equilibrium, resume ---
e8 = mb.Engine(1, 10000)
e8.halt()
assert e8.is_halted()
e8.submit_limit(mb.Side.Sell, 99, 10)
e8.submit_limit(mb.Side.Sell, 100, 10)
e8.submit_limit(mb.Side.Buy, 102, 10)
e8.submit_limit(mb.Side.Buy, 101, 10)
assert e8.take_trades() == []                          # call phase: no matching
assert e8.best_bid() == 102 and e8.best_ask() == 99    # crossed, standing
assert e8.uncross() == 20
prints = e8.take_trades()
assert len(prints) == 2 and all(t.price == 100 for t in prints)
e8.resume()
assert not e8.is_halted()
assert e8.last_trade() == 100

# --- Avellaneda-Stoikov ---
p = mb.ASParams()
p.gamma = 0.1
as_ = mb.AvellanedaStoikov(p)
q0 = as_.quotes(1000.0, 0, 0.0)
ql = as_.quotes(1000.0, 100, 0.0)
assert q0.bid < q0.ask
assert ql.bid < q0.bid and ql.ask < q0.ask  # long inventory skews down
assert as_.reservation_price(1000.0, 100, 0.0) < 1000.0

# --- RL quoter: uncrossed quotes, deterministic Q update ---
rl = mb.RLQuoter()
q = rl.quotes(1000.0, 0)
assert q.ask > q.bid
rp = mb.RLParams()
fresh = mb.RLQuoter(rp)
fresh.quotes(1000.0, 0)
fresh.learn(10.0, 0)
assert abs(fresh.q_value(fresh.bucket(0), 0) - rp.alpha * 10.0) < 1e-12

print("OK: bindings smoke test passed")
