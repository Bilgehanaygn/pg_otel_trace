-- Schema: customers + orders (join-heavy workload)
DROP TABLE IF EXISTS orders CASCADE;
DROP TABLE IF EXISTS customers CASCADE;

CREATE TABLE customers (
    customer_id   SERIAL PRIMARY KEY,
    region        TEXT NOT NULL,
    tier          TEXT NOT NULL CHECK (tier IN ('bronze', 'silver', 'gold', 'platinum')),
    signup_date   DATE NOT NULL,
    credit_limit  NUMERIC(12, 2) NOT NULL,
    is_active     BOOLEAN NOT NULL DEFAULT true
);

CREATE TABLE orders (
    order_id      SERIAL PRIMARY KEY,
    customer_id   INTEGER NOT NULL REFERENCES customers(customer_id),
    order_date    DATE NOT NULL,
    status        TEXT NOT NULL CHECK (status IN ('pending', 'shipped', 'delivered', 'cancelled')),
    amount        NUMERIC(12, 2) NOT NULL,
    discount_pct  NUMERIC(5, 2) NOT NULL DEFAULT 0
);

-- Bulk insert customers (50k rows)
INSERT INTO customers (region, tier, signup_date, credit_limit, is_active)
SELECT
    (ARRAY['north', 'south', 'east', 'west', 'central'])[1 + (g % 5)],
    (ARRAY['bronze', 'silver', 'gold', 'platinum'])[1 + (g % 4)],
    DATE '2018-01-01' + (g % 2000),
    500 + (g % 9500) + (random() * 100)::numeric(12, 2),
    (g % 17) <> 0
FROM generate_series(1, 50000) AS g;

-- Bulk insert orders (200k rows)
INSERT INTO orders (customer_id, order_date, status, amount, discount_pct)
SELECT
    1 + (g % 50000),
    DATE '2020-01-01' + (g % 1500),
    (ARRAY['pending', 'shipped', 'delivered', 'cancelled'])[1 + (g % 4)],
    (10 + (g % 500) + random() * 50)::numeric(12, 2),
    (g % 30)::numeric(5, 2)
FROM generate_series(1, 200000) AS g;

ANALYZE customers;
ANALYZE orders;

-- Indexes for nested-loop / hash / index scans + sort
CREATE INDEX idx_customers_region_tier ON customers (region, tier) WHERE is_active;
CREATE INDEX idx_customers_signup ON customers (signup_date);
CREATE INDEX idx_orders_customer_date ON orders (customer_id, order_date);
CREATE INDEX idx_orders_status_amount ON orders (status, amount);
CREATE INDEX idx_orders_date ON orders (order_date);

ANALYZE customers;
ANALYZE orders;

-- Complex query: CTEs, joins, filters, aggregation, window, subquery, sort
EXPLAIN (ANALYZE, BUFFERS, FORMAT TEXT)
WITH active_customers AS (
    SELECT
        c.customer_id,
        c.region,
        c.tier,
        c.signup_date,
        c.credit_limit
    FROM customers c
    WHERE c.is_active
      AND c.tier IN ('gold', 'platinum')
      AND c.signup_date >= DATE '2020-01-01'
),
order_agg AS (
    SELECT
        o.customer_id,
        COUNT(*) AS order_count,
        SUM(o.amount * (1 - o.discount_pct / 100)) AS revenue,
        AVG(o.amount) AS avg_amount,
        MAX(o.order_date) AS last_order_date
    FROM orders o
    WHERE o.status IN ('shipped', 'delivered')
      AND o.order_date >= DATE '2022-01-01'
      AND o.amount > 25
    GROUP BY o.customer_id
    HAVING COUNT(*) >= 3
),
ranked AS (
    SELECT
        ac.customer_id,
        ac.region,
        ac.tier,
        ac.credit_limit,
        oa.order_count,
        oa.revenue,
        oa.avg_amount,
        oa.last_order_date,
        ROW_NUMBER() OVER (
            PARTITION BY ac.region
            ORDER BY oa.revenue DESC NULLS LAST
        ) AS revenue_rank_in_region
    FROM active_customers ac
    INNER JOIN order_agg oa ON oa.customer_id = ac.customer_id
)
SELECT
    r.region,
    r.tier,
    COUNT(*) AS customer_count,
    SUM(r.revenue) AS total_revenue,
    AVG(r.order_count) AS avg_orders_per_customer,
    MAX(r.revenue) AS top_customer_revenue
FROM ranked r
WHERE r.revenue_rank_in_region <= 50
  AND r.revenue > (
      SELECT AVG(sub.revenue)
      FROM ranked sub
      WHERE sub.region = r.region
  )
GROUP BY r.region, r.tier
HAVING SUM(r.revenue) > 10000
ORDER BY total_revenue DESC, r.region, r.tier;
