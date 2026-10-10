
awiki_path = /her/alicewiki
affspath = $(awiki_path)/research/performance/cache/af-fs-graph

all:
	clang++ -O0 -g -S -emit-llvm -gdwarf-4     *.cpp -o tsin.ll
	opt -load-pass-plugin=../build/FieldAnalysis.so -passes="field-analysis" tsin.ll -S -o targ.ll 
	clang++ targ.ll  ../build/runtime/libaffinity.a -lpthread  -static -g -gdwarf-4 -o t.exe
	rm *json
run:
	-mkdir csv
	-rm csv/* -rf
	cp t.exe csv/
	cd csv && ./t.exe
	cd csv &&  sort -m -n -t ',' -k 1 access_trace.*.csv | head -n 160000 > ../master_trace_small.csv

opt:
	rm -rf *.dot *.txt *.layout.txt
	$(affspath)/generator/target/debug/generator -i *.csv -o graph.dot --binary t*.exe --disable-self-weight
	@for file in graph.*.dot; do \
		case "$$file" in \
			*.fields.dot) \
				echo "👉 [Struct Packing] $$file ..."; \
				$(affspath)/package_fields/target/debug/package_fields "$$file" 1 1 10 64; \
				;; \
			graph.dot) \
				;; \
			*) \
				echo "👉 [Array Summarizing] $$file ..."; \
				$(affspath)/summarier/target/debug/summarier "$$file" 1 1 10 64; \
				;; \
		esac; \
	done
	@if [ -f graph.dot ]; then \
		echo "👉 [Globals Packing] graph.dot ..."; \
		$(affspath)/package_simple/target/debug/package_simple graph.dot 1 1 10 64; \
	fi
	
sum:
	python $(affspath)/summarize.py

clean:
	rm -rf *.dot *.txt *.layout.txt
