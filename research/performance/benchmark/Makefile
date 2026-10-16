
awiki_path = /her/alicewiki
affspath = $(awiki_path)/research/performance/cache/af-fs-graph
PASS_SO ?= $(affspath)/field_analysis/build/FieldAnalysis.so
RUNTIME_LIB ?= $(affspath)/field_analysis/build/runtime/libaffinity.a
DBG_VIEWTYPE ?= dbg-viewtype

SRCS ?= $(wildcard *.cpp)
BIN ?= t.exe
TRACE ?= trace.csv
TAU ?= 30000

K ?= 1
G ?= 15
GAMMA ?= 10
LINE ?= 64
PARAMS = $(K) $(G) $(GAMMA) $(LINE)

TU_LL = $(SRCS:.cpp=.ll)

TAG ?= $(shell date +%Y%m%d-%H%M%S)
REPORTDIR ?= reports
REPORT = $(REPORTDIR)/$(TAG).txt

.PHONY: all run opt report sum verify clean distclean

all: $(BIN)

$(BIN): targ.ll
	clang++ targ.ll $(RUNTIME_LIB) -lpthread -static -g -gdwarf-4 -o $(BIN)
	rm *json

targ.ll: tsin.ll
	opt -load-pass-plugin=$(PASS_SO) -passes="field-analysis" $< -S -o $@

tsin.ll: $(TU_LL)
	@if [ -z "$(strip $(TU_LL))" ]; then echo "✖ 当前目录没有 .cpp, 请用 make SRCS=xxx.cpp 指定"; exit 1; fi; \
	n=$$(grep -l '^define .* @main(' $(TU_LL) 2>/dev/null | wc -l); \
	if [ "$$n" -gt 1 ]; then \
		echo "✖ 检出 $$n 个定义 main() 的源文件, 它们不是同一程序的多个 TU:"; \
		grep -l '^define .* @main(' $(TU_LL) | tr '\n' ' '; echo; \
		echo "  请显式指定其中一个, 例如:  make SRCS=test_fields2.cpp"; \
		exit 1; \
	fi
	llvm-link $(TU_LL) -S -o $@

%.ll: %.cpp
	clang++ -O0 -g -S -emit-llvm -gdwarf-4 $< -o $@

$(TRACE): $(BIN)
	-mkdir csv
	-rm csv/* -rf
	cp $(BIN) csv/
	cd csv && ./$(notdir $(BIN))
	sort -m -n -t ',' -k 1 csv/access_trace.*.csv > $(TRACE)

run: $(TRACE)

opt: $(TRACE) $(BIN)
	rm -rf *.dot *.txt *.layout.txt *.decision.json
	$(affspath)/generator/target/debug/generator -i $(TRACE) -o graph.dot --binary $(BIN) --dbg-viewtype $(DBG_VIEWTYPE) --window $(TAU)
	@for file in graph.*.dot; do \
		case "$$file" in \
			*.fields.dot) \
				echo "👉 [Struct Packing] $$file ..."; \
				$(affspath)/package_fields/target/debug/package_fields "$$file" $(PARAMS); \
				;; \
			graph.dot) \
				;; \
			*) \
				echo "👉 [Array Summarizing] $$file ..."; \
				$(affspath)/summarier/target/debug/summarier "$$file" $(PARAMS); \
				;; \
		esac; \
	done
	@if [ -f graph.dot ]; then \
		echo "👉 [Globals Packing] graph.dot ..."; \
		$(affspath)/package_simple/target/debug/package_simple graph.dot $(PARAMS); \
	fi
	
report:
	@mkdir -p $(REPORTDIR)
	@{ \
		echo "run: src=$(SRCS) trace=$(TRACE) ($$(wc -l < $(TRACE) 2>/dev/null || echo 0) 行) k=$(K) g=$(G) gamma=$(GAMMA) line=$(LINE) window=$(TAU)ns"; \
		echo; \
		DBG_VIEWTYPE=$(DBG_VIEWTYPE) python $(affspath)/summarize.py .; \
	} | tee $(REPORT)

sum: report

verify: report
	@latest=$$(ls -t $(REPORTDIR)/*.txt | head -n 1); \
	echo "▶ 幂等自检: $$latest"; \
	printf "  transpose: %s\n" "$$(grep -c '执行轴置换' $$latest)"; \
	printf "  repack   : %s\n" "$$(grep -c '发现更优布局' $$latest)"; \
	printf "  keep     : %s\n" "$$(grep -c '维持现有' $$latest)"

clean:
	rm -rf *.dot *.txt *.layout.txt *.decision.json *csv reports *.ll *.exe

distclean: clean
	rm -rf tsin.ll targ.ll $(TU_LL) $(BIN) csv $(TRACE)
