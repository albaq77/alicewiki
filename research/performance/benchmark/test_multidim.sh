/mnt/hgfs/her/alicewiki/research/performance/cache/af-fs-graph/generator/target/debug/generator -i *.csv -o graph.dot --binary t*.exe --disable-self-weight
for file in graph.*.dot; do
    echo "Processing $file ..."
    /mnt/hgfs/her/alicewiki/research/performance/cache/af-fs-graph/summarier/target/debug/summarier "$file" 1 1 10 64
done
/mnt/hgfs/her/alicewiki/research/performance/cache/af-fs-graph/package_simple/target/debug/package_simple graph.dot 1 1 10 64

