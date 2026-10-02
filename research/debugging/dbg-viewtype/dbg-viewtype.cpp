//
#include <cpp/unisym>
#include <c/file.h>
#include <c/consio.h>
#undef _ASM
#include <fcntl.h>
#include <libelf.h>
#include <unistd.h>
#include <libdwarf/dwarf.h>
#include <libdwarf/libdwarf.h>
#include <iostream>
#include <unordered_map>
#include <vector>
#include <iomanip>

// 存储提取出的 DIE 信息
struct DieInfo {
    Dwarf_Half tag = 0;
    std::string name;
    Dwarf_Off type_offset = 0;      // 依赖类型的偏移量 (DW_AT_type)
    Dwarf_Unsigned mem_offset = 0;  // 结构体成员的相对偏移 (DW_AT_data_member_location)
    Dwarf_Addr addr = 0;            // 全局变量的绝对地址 (DW_AT_location)
    bool has_addr = false;          // 标记是否真的是全局/静态变量
    
    Dwarf_Off parent_offset = 0;    // 父节点偏移
    std::vector<Dwarf_Off> children; // 子节点偏移列表

	Dwarf_Signed upper_bound = -1;// 存储数组的最大索引 'Arr[MAX]'
    Dwarf_Unsigned byte_size = 0;   // 结构体或类型的总字节数 (DW_AT_byte_size)
};

// 全局的 DIE 映射表：Key 为全局偏移量
std::unordered_map<Dwarf_Off, DieInfo> die_map;

// sudo pacman -S libdwarf

void process_die_data(Dwarf_Debug dbg, Dwarf_Die die, DieInfo& info);

void traverse_die(Dwarf_Debug dbg, Dwarf_Die die, Dwarf_Off parent_offset) {
	Dwarf_Error err = nullptr;
	Dwarf_Off current_offset;
	dwarf_dieoffset(die, &current_offset, &err);

	// 将当前 DIE 注册到全局 Map 中，并建立父子双向链接
	die_map[current_offset].parent_offset = parent_offset;
	if (parent_offset != 0) {
		die_map[parent_offset].children.push_back(current_offset);
	}

	// 解析并存储当前节点数据
	process_die_data(dbg, die, die_map[current_offset]);

	// 递归子节点
	Dwarf_Die child_die = nullptr;
	if (dwarf_child(die, &child_die, &err) == DW_DLV_OK) {
		traverse_die(dbg, child_die, current_offset); // 传入 current_offset 作为父节点

		// 遍历兄弟节点
		Dwarf_Die sibling_die = nullptr;
		while (dwarf_siblingof_b(dbg, child_die, true, &sibling_die, &err) == DW_DLV_OK) {
			traverse_die(dbg, sibling_die, current_offset);
			dwarf_dealloc(dbg, child_die, DW_DLA_DIE);
			child_die = sibling_die;
		}
		dwarf_dealloc(dbg, child_die, DW_DLA_DIE);
	}
}

void process_die_data(Dwarf_Debug dbg, Dwarf_Die die, DieInfo& info) {
	Dwarf_Error err = nullptr;
	dwarf_tag(die, &info.tag, &err);

	Dwarf_Attribute attr;

	// 1. 获取名称 (DW_AT_name)
	if (dwarf_attr(die, DW_AT_name, &attr, &err) == DW_DLV_OK) {
		char* name = nullptr;
		if (dwarf_formstring(attr, &name, &err) == DW_DLV_OK) {
			info.name = name;
			dwarf_dealloc(dbg, name, DW_DLA_STRING);
		}
		dwarf_dealloc(dbg, attr, DW_DLA_ATTR);
	}

	// 2. 获取依赖类型 (DW_AT_type)
	if (dwarf_attr(die, DW_AT_type, &attr, &err) == DW_DLV_OK) {
		dwarf_global_formref(attr, &info.type_offset, &err);
		dwarf_dealloc(dbg, attr, DW_DLA_ATTR);
	}

	// 3. 提取结构体成员的偏移量 (DW_AT_data_member_location)
	if (info.tag == DW_TAG_member) {
		if (dwarf_attr(die, DW_AT_data_member_location, &attr, &err) == DW_DLV_OK) {
			// 在现代 DWARF4/5 中，成员偏移通常直接存为无符号整数
			dwarf_formudata(attr, &info.mem_offset, &err); 
			dwarf_dealloc(dbg, attr, DW_DLA_ATTR);
		}
	}

    // 4. 获取结构体或类型的总字节数 (DW_AT_byte_size)
    if (dwarf_attr(die, DW_AT_byte_size, &attr, &err) == DW_DLV_OK) {
        dwarf_formudata(attr, &info.byte_size, &err);
        dwarf_dealloc(dbg, attr, DW_DLA_ATTR);
    }

	// 5. 提取全局变量的内存地址 (DW_AT_location 中的 DW_OP_addr)
	if (info.tag == DW_TAG_variable) {
		if (dwarf_attr(die, DW_AT_location, &attr, &err) == DW_DLV_OK) {
			Dwarf_Loc_Head_c loc_head = nullptr;
			Dwarf_Unsigned loc_cnt = 0;
			
			// 1. 获取位置列表头
			if (dwarf_get_loclist_c(attr, &loc_head, &loc_cnt, &err) == DW_DLV_OK) {
				if (loc_cnt > 0) {
					// 严格匹配 _e 接口所需要的参数
					Dwarf_Small lle_value = 0;
					Dwarf_Unsigned rawlowpc = 0, rawhipc = 0;
					Dwarf_Bool debug_addr_unavailable = 0; 
					Dwarf_Addr lowpc_cooked = 0, hipc_cooked = 0;
					Dwarf_Unsigned locexpr_op_count = 0;
					Dwarf_Unsigned lle_bytecount = 0; // <-- 补上这个导致串位的罪魁祸首！
					Dwarf_Locdesc_c locdesc = nullptr;
					Dwarf_Small loclist_source = 0;
					Dwarf_Unsigned expression_offset = 0;
					Dwarf_Unsigned locdesc_offset = 0;
					
					// 2. 调用最新版 _e 接口 (完全按照参数顺序对齐)
					if (dwarf_get_locdesc_entry_e(
							loc_head, 0, // 仅取第一个条目
							&lle_value, &rawlowpc, &rawhipc,
							&debug_addr_unavailable, &lowpc_cooked, &hipc_cooked,
							&locexpr_op_count,
							&lle_bytecount,      // <-- 放入正确的位置
							&locdesc,
							&loclist_source, &expression_offset, &locdesc_offset,
							&err) == DW_DLV_OK) {
						
						if (locexpr_op_count > 0) {
							Dwarf_Small op = 0;
							Dwarf_Unsigned op1 = 0, op2 = 0, op3 = 0, offset = 0;
							
							// 3. 读取具体的操作码
							if (dwarf_get_location_op_value_c(locdesc, 0, &op, &op1, &op2, &op3, &offset, &err) == DW_DLV_OK) {
								if (op == DW_OP_addr) {
									// 拿到全局变量的虚拟内存地址！
									info.addr = op1; 
									info.has_addr = true;
								}
							}
						}
					}
				}
				// 4. 清理位置列表内存
				dwarf_dealloc_loc_head_c(loc_head);
			}
			dwarf_dealloc(dbg, attr, DW_DLA_ATTR);
		}
	}

	// 5. 提取数组的子范围上界 (DW_AT_upper_bound) 或者个数 (DW_AT_count)
	if (info.tag == DW_TAG_subrange_type) {
		if (dwarf_attr(die, DW_AT_upper_bound, &attr, &err) == DW_DLV_OK) {
			Dwarf_Unsigned bound_u = 0;
            Dwarf_Signed bound_s = 0;
			if (dwarf_formudata(attr, &bound_u, &err) == DW_DLV_OK) {
				info.upper_bound = bound_u;
			} else if (dwarf_formsdata(attr, &bound_s, &err) == DW_DLV_OK) {
                info.upper_bound = bound_s;
            }
			dwarf_dealloc(dbg, attr, DW_DLA_ATTR);
		} else if (dwarf_attr(die, DW_AT_count, &attr, &err) == DW_DLV_OK) {
            Dwarf_Unsigned count_u = 0;
            Dwarf_Signed count_s = 0;
			if (dwarf_formudata(attr, &count_u, &err) == DW_DLV_OK) {
				info.upper_bound = count_u - 1;
			} else if (dwarf_formsdata(attr, &count_s, &err) == DW_DLV_OK) {
                info.upper_bound = count_s - 1;
            }
			dwarf_dealloc(dbg, attr, DW_DLA_ATTR);
        }
	}
}

void process_type_die(Dwarf_Debug dbg, Dwarf_Die die) {
	Dwarf_Half tag;
	Dwarf_Error err = nullptr;

	// 获取当前 DIE 的 Tag
	if (dwarf_tag(die, &tag, &err) != DW_DLV_OK) return;

	// 过滤：我们只关心类型相关的 Tag
	if (tag != DW_TAG_base_type && 
		tag != DW_TAG_structure_type && 
		tag != DW_TAG_union_type &&    // <--- 确保放行联合体
		tag != DW_TAG_pointer_type &&
		tag != DW_TAG_typedef &&
		tag != DW_TAG_const_type &&       // <--- 确保 const 不被扔掉
		tag != DW_TAG_volatile_type &&    // <--- 确保 volatile 不被扔掉
		tag != DW_TAG_variable &&
		tag != DW_TAG_member &&
		tag != DW_TAG_array_type &&    // <--- 确保放行数组类型
		tag != DW_TAG_subrange_type) { // <--- 确保放行子范围类型
		return; 
	}

	// --- 获取全局偏移量 (作为你内部哈希表的 Key) ---
	Dwarf_Off die_offset;
	dwarf_dieoffset(die, &die_offset, &err);

	// --- 获取名字 (DW_AT_name) ---
	Dwarf_Attribute attr;
	if (dwarf_attr(die, DW_AT_name, &attr, &err) == DW_DLV_OK) {
		char* name = nullptr;
		if (dwarf_formstring(attr, &name, &err) == DW_DLV_OK) {
			std::cout << "找到类型: " << name << " (Offset: " << die_offset << ")" << std::endl;
			dwarf_dealloc(dbg, name, DW_DLA_STRING); // 释放名字字符串
		}
		dwarf_dealloc(dbg, attr, DW_DLA_ATTR);
	}

	// --- 获取依赖类型 (DW_AT_type，比如指针指向什么类型) ---
	if (dwarf_attr(die, DW_AT_type, &attr, &err) == DW_DLV_OK) {
		Dwarf_Off target_offset;
		// dwarf_global_formref 会把局部偏移自动转为全局偏移，非常实用
		if (dwarf_global_formref(attr, &target_offset, &err) == DW_DLV_OK) {
			std::cout << "  依赖类型 Offset: " << target_offset << std::endl;
		}
		dwarf_dealloc(dbg, attr, DW_DLA_ATTR);
	}
}

std::string get_type_name(Dwarf_Off type_offset) {
	if (type_offset == 0 || die_map.find(type_offset) == die_map.end()) {
		return "void"; 
	}

	DieInfo& tInfo = die_map[type_offset];

	if (tInfo.tag == DW_TAG_pointer_type) {
		return get_type_name(tInfo.type_offset) + "*";
	} else if (tInfo.tag == DW_TAG_const_type) {
		return "const " + get_type_name(tInfo.type_offset);
	} else if (tInfo.tag == DW_TAG_volatile_type) {  // <--- 新增这一段
		return "volatile " + get_type_name(tInfo.type_offset);
	} else if (tInfo.tag == DW_TAG_typedef) {
		return tInfo.name; 
	} 
	// --- 新增：处理数组类型 ---
	else if (tInfo.tag == DW_TAG_array_type) {
		// 1. 获取它的基础元素类型 (例如 T)
		std::string base_name = get_type_name(tInfo.type_offset);
		std::string array_suffix = "";
		
		// 2. 遍历它的子节点，寻找 DW_TAG_subrange_type
		for (Dwarf_Off child_off : tInfo.children) {
			DieInfo& childInfo = die_map[child_off];
			if (childInfo.tag == DW_TAG_subrange_type) {
				if (childInfo.upper_bound != -1) {
					// 数组大小 = 上界 + 1 (例如上界127，大小就是128)
					array_suffix += "[" + std::to_string(childInfo.upper_bound + 1) + "]";
				} else {
					array_suffix += "[]";
				}
			}
		}
		return base_name + array_suffix;
	}
	// --- 新增：动态展开匿名结构体/联合体 ---
	else if (tInfo.tag == DW_TAG_structure_type || tInfo.tag == DW_TAG_union_type) {
		if (tInfo.name.empty()) {
			std::string inline_def = (tInfo.tag == DW_TAG_union_type) ? "union { " : "struct { ";
			// 遍历匿名结构体内部的成员
			for (Dwarf_Off child_off : tInfo.children) {
				DieInfo& memInfo = die_map[child_off];
				if (memInfo.tag == DW_TAG_member) {
					// 如果内部成员也没有名字，给个占位符
					std::string mem_name = memInfo.name.empty() ? "(anon)" : memInfo.name;
					inline_def += get_type_name(memInfo.type_offset) + " " + mem_name + "; ";
				}
			}
			inline_def += "}";
			return inline_def;
		}
		return tInfo.name; // 有名字的结构体直接返回名字
	}
	return tInfo.name.empty() ? "anonymous" : tInfo.name;
}

// 辅助函数：穿透 typedef 和 const，找到最底层的真实类型 DIE 偏移量
Dwarf_Off get_real_type(Dwarf_Off type_offset) {
	while (type_offset != 0 && die_map.find(type_offset) != die_map.end()) {
		Dwarf_Half tag = die_map[type_offset].tag;
		if (tag == DW_TAG_typedef || tag == DW_TAG_const_type || tag == DW_TAG_volatile_type) {
			type_offset = die_map[type_offset].type_offset; // 继续往下找
		} else {
			break; // 找到实打实的 struct/union/base_type 了
		}
	}
	return type_offset;
}

// 递归展开结构体成员的引擎
void print_struct_members_recursively(uni::OstreamTrait& out, Dwarf_Off struct_type_offset, Dwarf_Unsigned base_offset, const std::string& parent_prefix) {
	if (die_map.find(struct_type_offset) == die_map.end()) return;
	DieInfo& info = die_map[struct_type_offset];

	for (Dwarf_Off child_off : info.children) {
		DieInfo& memberInfo = die_map[child_off];
		if (memberInfo.tag == DW_TAG_member) {
			
			// 1. 计算绝对偏移量 = 父基址 + 当前相对偏移
			Dwarf_Unsigned abs_offset = base_offset + memberInfo.mem_offset;

			// 2. 拼接名称链
			std::string print_name;       // 当前行要显示的名字
			std::string next_prefix;      // 传给下一层的父前缀
			
			if (memberInfo.name.empty()) {
				print_name = "(anon)";
				next_prefix = parent_prefix; // 匿名节点不增加前缀，直接透传 (例如 a 还是叫 a)
			} else {
				// 如果有父前缀，拼成 t.field1 的形式
				print_name = parent_prefix.empty() ? memberInfo.name : parent_prefix + "." + memberInfo.name;
				next_prefix = print_name;
			}

			// 3. 打印当前成员节点本身
			out.OutFormat("\t%u\t%s\t\'%s\'\n", abs_offset, print_name.c_str(), get_type_name(memberInfo.type_offset).c_str());
			// std::cout << "    " << std::left << std::setw(6) << abs_offset
			// 			<< std::setw(15) << print_name
			// 			<< get_type_name(memberInfo.type_offset) << "\n";

			// 4. 判断该成员是否是复合类型，如果是，继续递归展开！
			Dwarf_Off real_type_offset = get_real_type(memberInfo.type_offset);
			if (real_type_offset != 0 && die_map.find(real_type_offset) != die_map.end()) {
				Dwarf_Half type_tag = die_map[real_type_offset].tag;
				
				// ⚠️ 极度危险警告：只展开值类型的 struct/union，绝不能展开 pointer 指针类型，否则 T* 指向 T 会无限死循环！
				if (type_tag == DW_TAG_structure_type || type_tag == DW_TAG_union_type) {
					print_struct_members_recursively(out, real_type_offset, abs_offset, next_prefix);
				}
			}
		}
	}
}

int main(int argc, char* argv[]) {
	if (argc < 2) {
		std::cerr << "请提供 DWARF 文件路径作为参数" << std::endl;
		std::cerr << "dbg-viewtype <dwarf-file> (type-outfile) (var-outfile)" << std::endl;
		return 1;
	}
	int fd = open(argv[1], O_RDONLY);
	if (fd < 0) {
		std::cerr << "打开文件失败，请检查路径" << std::endl;
		return 1;
	}
	Dwarf_Debug dbg = nullptr;
	Dwarf_Error err = nullptr;

	// 初始化 libdwarf，成功返回 DW_DLV_OK
	if (dwarf_init_b(fd, 0, nullptr, nullptr, &dbg, &err) != DW_DLV_OK) {
		std::cerr << "初始化 DWARF 失败" << std::endl;
		close(fd);
		return 1;
	}
	std::cout << "DWARF 初始化成功，开始解析喵..." << std::endl;

	// 遍历编译单元 (CU) 的循环引擎
	Dwarf_Unsigned cu_header_length = 0;
	Dwarf_Half version_stamp = 0;
	Dwarf_Off abbrev_offset = 0;
	Dwarf_Half address_size = 0;
	Dwarf_Half length_size = 0;
	Dwarf_Half extension_size = 0;
	Dwarf_Sig8 type_signature;
	Dwarf_Unsigned typeoffset = 0;
	Dwarf_Half header_cu_type = 0;

	// 循环游走于各个 CU 之间
	while (dwarf_next_cu_header_d(dbg, true, &cu_header_length, &version_stamp, 
									&abbrev_offset, &address_size, &length_size, 
									&extension_size, &type_signature, &typeoffset, 
									nullptr, &header_cu_type, &err) == DW_DLV_OK) {
		
		Dwarf_Die no_die = nullptr;
		Dwarf_Die cu_die = nullptr;
		
		// 获取当前 CU 的第一个 DIE (根节点)
		if (dwarf_siblingof_b(dbg, no_die, true, &cu_die, &err) == DW_DLV_OK) {
			// 启动深度优先遍历！
			traverse_die(dbg, cu_die, 0); 
			
			// 释放 CU 根节点内存
			dwarf_dealloc(dbg, cu_die, DW_DLA_DIE);
		}
	}

	std::cout << "解析完成喵~" << std::endl;

	// "<类型>\n";
	// "    <偏移>  <名称>  <类型>\n";
	// "\n======= 类型 =======" << std::endl;


	using namespace uni;
	OstreamTrait* pout = &uni::Console;
	HostFile* hf = nullptr;
	if (argc > 2) {
		hf = new HostFile(argv[2], FileOpenType::Write);
		if (hf) pout = hf;
	}
	for (auto const& [offset, info] : die_map) {
		// 如果是结构体，并且有名字
		if (info.tag == DW_TAG_structure_type && !info.name.empty()) {
			pout->OutFormat("%s\t%llu\n", info.name.c_str(), info.byte_size);
			print_struct_members_recursively(*pout, offset, 0, "");
		}
	}
	if (hf) delete hf;
	pout = &uni::Console;
	if (argc > 3) {
		hf = new HostFile(argv[3], FileOpenType::Write);
		if (hf) pout = hf;
	} 
	// "======= 全局变量 =======" << std::endl;
	for (auto const& [offset, info] : die_map) {
		// 如果是变量，并且我们成功提取到了它的物理地址
		if (info.tag == DW_TAG_variable && info.has_addr) {
			pout->OutFormat("%s\t%u\t\'%s\'\n", info.name.c_str(), info.addr, get_type_name(info.type_offset).c_str());
		}
	}

	dwarf_finish(dbg);
	close(fd);

}
