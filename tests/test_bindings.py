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
