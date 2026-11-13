//! A forgiving XML scanner. Mod XML is often not well-formed (stray `&`, mismatched
//! end tags), and the game's own parser shrugs that off, so a strict parser would hide
//! definitions the game happily loads. This one never fails and records byte spans so
//! values can be edited in place without reformatting the file.

#[derive(Clone, Debug, Default)]
pub struct Elem {
    pub tag: String,
    pub attrs: Vec<(String, String)>,
    /// Whole element, from `<` of the start tag to `>` of the end tag.
    pub span: (usize, usize),
    /// Content between the start and end tags (empty for `<x/>`).
    pub inner: (usize, usize),
    pub self_closing: bool,
    pub children: Vec<Elem>,
    /// Direct text content, entity-decoded and trimmed.
    pub text: String,
}

impl Elem {
    pub fn attr(&self, name: &str) -> Option<&str> {
        self.attrs.iter().find(|(k, _)| k.eq_ignore_ascii_case(name)).map(|(_, v)| v.as_str())
    }
    pub fn child(&self, tag: &str) -> Option<&Elem> {
        self.children.iter().find(|c| c.tag.eq_ignore_ascii_case(tag))
    }
    pub fn child_text(&self, tag: &str) -> Option<&str> {
        self.child(tag).map(|c| c.text.as_str())
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Encoding {
    Utf8,
    Utf8Bom,
    Windows1252,
    /// Readable but not written back.
    Utf16,
}

pub fn decode(bytes: &[u8]) -> (String, Encoding) {
    if bytes.starts_with(&[0xEF, 0xBB, 0xBF]) {
        return (String::from_utf8_lossy(&bytes[3..]).into_owned(), Encoding::Utf8Bom);
    }
    if bytes.starts_with(&[0xFF, 0xFE]) || bytes.starts_with(&[0xFE, 0xFF]) {
        let enc = if bytes[0] == 0xFF { encoding_rs::UTF_16LE } else { encoding_rs::UTF_16BE };
        let (s, _, _) = enc.decode(bytes);
        return (s.into_owned(), Encoding::Utf16);
    }
    match std::str::from_utf8(bytes) {
        Ok(s) => (s.to_owned(), Encoding::Utf8),
        Err(_) => {
            let (s, _, _) = encoding_rs::WINDOWS_1252.decode(bytes);
            (s.into_owned(), Encoding::Windows1252)
        }
    }
}

pub fn encode(text: &str, enc: Encoding) -> Option<Vec<u8>> {
    match enc {
        Encoding::Utf8 => Some(text.as_bytes().to_vec()),
        Encoding::Utf8Bom => {
            let mut v = vec![0xEF, 0xBB, 0xBF];
            v.extend_from_slice(text.as_bytes());
            Some(v)
        }
        Encoding::Windows1252 => {
            let (bytes, _, lossy) = encoding_rs::WINDOWS_1252.encode(text);
            (!lossy).then(|| bytes.into_owned())
        }
        Encoding::Utf16 => None,
    }
}

pub fn unescape(s: &str) -> String {
    if !s.contains('&') {
        return s.to_owned();
    }
    let mut out = String::with_capacity(s.len());
    let mut rest = s;
    while let Some(i) = rest.find('&') {
        out.push_str(&rest[..i]);
        rest = &rest[i..];
        let end = rest[..rest.len().min(12)].find(';');
        let rep = end.and_then(|e| {
            let name = &rest[1..e];
            let c = match name {
                "amp" => Some('&'),
                "lt" => Some('<'),
                "gt" => Some('>'),
                "quot" => Some('"'),
                "apos" => Some('\''),
                _ if name.starts_with("#x") || name.starts_with("#X") => {
                    u32::from_str_radix(&name[2..], 16).ok().and_then(char::from_u32)
                }
                _ if name.starts_with('#') => name[1..].parse().ok().and_then(char::from_u32),
                _ => None,
            };
            c.map(|c| (c, e + 1))
        });
        match rep {
            Some((c, used)) => {
                out.push(c);
                rest = &rest[used..];
            }
            None => {
                out.push('&');
                rest = &rest[1..];
            }
        }
    }
    out.push_str(rest);
    out
}

pub fn escape(s: &str) -> String {
    s.replace('&', "&amp;").replace('<', "&lt;").replace('>', "&gt;")
}

struct Open {
    elem: Elem,
    raw_text: String,
}

fn finish(mut o: Open, end: usize, inner_end: usize) -> Elem {
    o.elem.span.1 = end;
    o.elem.inner.1 = inner_end.max(o.elem.inner.0);
    o.elem.text = unescape(o.raw_text.trim());
    o.elem
}

/// Parses `text` into its top-level elements (normally a single root).
pub fn parse(text: &str) -> Vec<Elem> {
    let b = text.as_bytes();
    let mut roots: Vec<Elem> = Vec::new();
    let mut stack: Vec<Open> = Vec::new();
    let mut i = 0usize;

    fn attach(stack: &mut Vec<Open>, roots: &mut Vec<Elem>, e: Elem) {
        match stack.last_mut() {
            Some(p) => p.elem.children.push(e),
            None => roots.push(e),
        }
    }

    while i < b.len() {
        if b[i] != b'<' {
            let next = text[i..].find('<').map(|n| i + n).unwrap_or(b.len());
            if let Some(top) = stack.last_mut() {
                top.raw_text.push_str(&text[i..next]);
            }
            i = next;
            continue;
        }
        let rest = &text[i..];
        if rest.starts_with("<!--") {
            i = rest.find("-->").map(|n| i + n + 3).unwrap_or(b.len());
        } else if rest.starts_with("<![CDATA[") {
            let end = rest.find("]]>").map(|n| i + n).unwrap_or(b.len());
            if let Some(top) = stack.last_mut() {
                top.raw_text.push_str(&text[i + 9..end]);
            }
            i = (end + 3).min(b.len());
        } else if rest.starts_with("<?") {
            i = rest.find("?>").map(|n| i + n + 2).unwrap_or(b.len());
        } else if rest.starts_with("<!") {
            i = rest.find('>').map(|n| i + n + 1).unwrap_or(b.len());
        } else if rest.starts_with("</") {
            let close = rest.find('>').map(|n| i + n).unwrap_or(b.len());
            let name = text[i + 2..close].trim();
            let after = (close + 1).min(b.len());
            if let Some(pos) = stack.iter().rposition(|o| o.elem.tag.eq_ignore_ascii_case(name)) {
                // Anything still open above the match was never closed: end it here.
                while stack.len() > pos + 1 {
                    let o = stack.pop().unwrap();
                    let e = finish(o, i, i);
                    attach(&mut stack, &mut roots, e);
                }
                let o = stack.pop().unwrap();
                let e = finish(o, after, i);
                attach(&mut stack, &mut roots, e);
            }
            i = after;
        } else if rest.len() > 1 && (b[i + 1].is_ascii_alphabetic() || b[i + 1] == b'_') {
            // Start tag: name, then attributes up to the first '>' outside quotes.
            let mut j = i + 1;
            while j < b.len() && !b[j].is_ascii_whitespace() && b[j] != b'>' && b[j] != b'/' {
                j += 1;
            }
            let tag = text[i + 1..j].to_owned();
            let mut attrs = Vec::new();
            let mut self_closing = false;
            loop {
                while j < b.len() && b[j].is_ascii_whitespace() {
                    j += 1;
                }
                if j >= b.len() {
                    break;
                }
                if b[j] == b'>' {
                    j += 1;
                    break;
                }
                if b[j] == b'/' {
                    self_closing = j + 1 < b.len() && b[j + 1] == b'>';
                    j += 1;
                    continue;
                }
                if b[j] == b'<' {
                    break; // broken tag; let the outer loop resync
                }
                let ks = j;
                while j < b.len() && !b[j].is_ascii_whitespace() && !matches!(b[j], b'=' | b'>' | b'/') {
                    j += 1;
                }
                let key = text[ks..j].to_owned();
                while j < b.len() && b[j].is_ascii_whitespace() {
                    j += 1;
                }
                let mut val = String::new();
                if j < b.len() && b[j] == b'=' {
                    j += 1;
                    while j < b.len() && b[j].is_ascii_whitespace() {
                        j += 1;
                    }
                    if j < b.len() && (b[j] == b'"' || b[j] == b'\'') {
                        let q = b[j];
                        let vs = j + 1;
                        j = vs;
                        while j < b.len() && b[j] != q {
                            j += 1;
                        }
                        val = unescape(&text[vs..j]);
                        j = (j + 1).min(b.len());
                    } else {
                        let vs = j;
                        while j < b.len() && !b[j].is_ascii_whitespace() && b[j] != b'>' {
                            j += 1;
                        }
                        val = unescape(&text[vs..j]);
                    }
                }
                if !key.is_empty() {
                    attrs.push((key, val));
                }
            }
            let elem = Elem { tag, attrs, span: (i, j), inner: (j, j), self_closing, ..Default::default() };
            if self_closing {
                attach(&mut stack, &mut roots, elem);
            } else {
                stack.push(Open { elem, raw_text: String::new() });
            }
            i = j;
        } else {
            if let Some(top) = stack.last_mut() {
                top.raw_text.push('<');
            }
            i += 1;
        }
    }
    while let Some(o) = stack.pop() {
        let e = finish(o, b.len(), b.len());
        attach(&mut stack, &mut roots, e);
    }
    roots
}
