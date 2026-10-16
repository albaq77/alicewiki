// Shared LP64 type rules used by the producer and both packers. Unknown types
// must be resolved through DWARF, never silently guessed to be eight bytes.
pub fn scalar_layout(name: &str) -> Option<(u64, u64)> {
    let compact = name
        .split_whitespace()
        .filter(|s| !matches!(*s, "const" | "volatile" | "restrict" | "_Atomic"))
        .collect::<String>();
    let size = match compact.as_str() {
        "bool" | "_Bool" | "char" | "signedchar" | "unsignedchar" | "int8_t" | "uint8_t" => 1,
        "short" | "shortint" | "signedshort" | "signedshortint" | "unsignedshort"
        | "unsignedshortint" | "int16_t" | "uint16_t" => 2,
        "int" | "signed" | "signedint" | "unsigned" | "unsignedint" | "int32_t" | "uint32_t"
        | "float" => 4,
        "long"
        | "longint"
        | "signedlong"
        | "signedlongint"
        | "unsignedlong"
        | "unsignedlongint"
        | "longlong"
        | "longlongint"
        | "signedlonglong"
        | "signedlonglongint"
        | "unsignedlonglong"
        | "unsignedlonglongint"
        | "int64_t"
        | "uint64_t"
        | "double" => 8,
        "longdouble" | "__int128" | "unsigned__int128" => 16,
        _ if compact.ends_with('*') => 8,
        _ => return None,
    };
    Some((size, size))
}

pub fn type_layout(name: &str) -> Option<(u64, u64)> {
    let mut base = name.trim();
    let mut count = 1u64;
    while base.ends_with(']') {
        let start = base.rfind('[')?;
        count = count.checked_mul(base[start + 1..base.len() - 1].trim().parse::<u64>().ok()?)?;
        base = base[..start].trim();
    }
    let (size, alignment) = scalar_layout(base).or_else(|| {
        // Also accept declarations such as `volatile int arr[10]`.
        let (typ, _) = base.rsplit_once(char::is_whitespace)?;
        scalar_layout(typ)
    })?;
    Some((size.checked_mul(count)?, alignment))
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn sizes_and_alignment_are_shared() {
        assert_eq!(type_layout("const volatile const int[10]"), Some((40, 4)));
        assert_eq!(type_layout("volatile int arr[10]"), Some((40, 4)));
        assert_eq!(type_layout("unsigned long long[2][3]"), Some((48, 8)));
        assert_eq!(type_layout("longint"), Some((8, 8)));
        assert_eq!(type_layout("long double"), Some((16, 16)));
        assert_eq!(type_layout("UnresolvedStruct"), None);
    }
}
