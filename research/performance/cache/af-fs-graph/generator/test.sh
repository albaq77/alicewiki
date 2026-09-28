cargo run -- -i ./.test/trace.csv -o .test/graph.dot --binary ./.test/sharing_bench
xdot .test/graph.dot

