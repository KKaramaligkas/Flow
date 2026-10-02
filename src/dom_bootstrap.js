/* PSP reader DOM. No filesystem, process, or native module bindings. */
(() => {
'use strict';
const nodes=__seed, wrappers=new Map(), listeners=new Map();
delete globalThis.__seed;
const raw=n=>nodes[n._id], wrap=id=>id==null?null:wrappers.get(id)||new Node(id);
const descendants=id=>{const out=[],seen=new Set();function visit(i,d){if(d>64||seen.has(i))throw Error('DOM depth/cycle');seen.add(i);for(const c of nodes[i].children){out.push(c);visit(c,d+1);}}visit(id,0);return out;};
function create(tag,text=''){if(nodes.length>=2048)throw Error('PSP DOM node limit');const id=nodes.length;nodes.push({tag,text,attrs:{},children:[]});return wrap(id);}
function simple(n,s){const m=s.match(/^(\*|[\w-]+)?((?:[.#][\w-]+)*)$/);if(!m)return false;if(m[1]&&m[1]!=='*'&&m[1].toLowerCase()!==n.tag)return false;return [...m[2].matchAll(/([.#])([\w-]+)/g)].every(([,t,v])=>t==='#'?n.attrs.id===v:(n.attrs.class||'').split(/\s+/).includes(v));}
function matches(node,selector){return selector.split(',').some(s=>{const parts=s.trim().split(/\s+/);let n=node;if(!simple(raw(n),parts.pop()))return false;while(parts.length){const p=parts.pop();n=n.parentNode;while(n&&!simple(raw(n),p))n=n.parentNode;if(!n)return false;}return true;});}
function decode(s){return s.replace(/&(#x[\da-f]+|#\d+|amp|lt|gt|quot|apos|nbsp);/gi,(_,n)=>n[0]==='#'?String.fromCodePoint(Math.min(0x10ffff,parseInt(n.slice(n[1].toLowerCase()==='x'?2:1),n[1].toLowerCase()==='x'?16:10))):({amp:'&',lt:'<',gt:'>',quot:'"',apos:"'",nbsp:' '})[n.toLowerCase()]);}
function fragment(parent,html){if(html.length>262144)throw Error('PSP HTML limit');const stack=[parent];const tokens=html.match(/<!--[\s\S]*?-->|<[^>]*>|[^<]+|</g)||[];for(const token of tokens){if(token.startsWith('<!--'))continue;if(token.startsWith('</')){if(stack.length>1)stack.pop();continue;}if(token[0]==='<'){const m=token.match(/^<([\w-]+)/);if(!m)continue;const n=create(m[1].toLowerCase());for(const a of token.slice(m[0].length).matchAll(/([\w:-]+)(?:\s*=\s*(?:"([^"]*)"|'([^']*)'|([^\s>]+)))?/g))n.setAttribute(a[1],decode(a[2]??a[3]??a[4]??''));stack.at(-1).appendChild(n);if(!/^(br|hr|img|input|meta|link|base)$/.test(raw(n).tag)&&!token.endsWith('/>')){if(stack.length>=64)throw Error('DOM depth');stack.push(n);}}else stack.at(-1).appendChild(create('#text',decode(token)));}}
class Node {
 constructor(id){Object.defineProperty(this,'_id',{value:id});wrappers.set(id,this);}
 get nodeType(){return raw(this).tag==='#document'?9:raw(this).tag==='#text'?3:1;}
 get nodeName(){return raw(this).tag.toUpperCase();}get tagName(){return this.nodeName;}
 get childNodes(){return raw(this).children.map(wrap);}get children(){return this.childNodes.filter(n=>n.nodeType===1);}get firstChild(){return this.childNodes[0]||null;}get lastChild(){return this.childNodes.at(-1)||null;}
 get parentNode(){const i=nodes.findIndex(n=>n.children.includes(this._id));return i<0?null:wrap(i);}get parentElement(){return this.parentNode;}
 get nextSibling(){const p=this.parentNode;if(!p)return null;const c=raw(p).children;return wrap(c[c.indexOf(this._id)+1]);}
 get textContent(){if(this.nodeType===3||raw(this).tag==='style')return raw(this).text;return this.childNodes.map(n=>n.textContent).join('');}
 set textContent(v){v=String(v);if(v.length>262144)throw Error('PSP text limit');if(this.nodeType===3||raw(this).tag==='style')raw(this).text=v;else{raw(this).children=[];if(v)this.appendChild(create('#text',v));}}
 get innerText(){return this.textContent;}set innerText(v){this.textContent=v;}
 get innerHTML(){return this.childNodes.map(n=>n.nodeType===3?n.textContent:`<${raw(n).tag}>${n.innerHTML}</${raw(n).tag}>`).join('');}
 set innerHTML(v){raw(this).children=[];fragment(this,String(v));}
 appendChild(n){return this.insertBefore(n,null);}append(...ns){for(const n of ns)this.appendChild(n instanceof Node?n:create('#text',String(n)));}
 insertBefore(n,ref){if(!(n instanceof Node)||n._id===0||n===this)throw Error('Invalid DOM insertion');for(let p=this,d=0;p;p=p.parentNode){if(p===n||++d>64)throw Error('DOM cycle');}let at=ref==null?raw(this).children.length:raw(this).children.indexOf(ref._id);if(at<0)throw Error('Not a child');n.remove();if(ref)at=raw(this).children.indexOf(ref._id);raw(this).children.splice(at,0,n._id);return n;}
 removeChild(n){const i=raw(this).children.indexOf(n._id);if(i<0)throw Error('Not a child');raw(this).children.splice(i,1);return n;}remove(){this.parentNode?.removeChild(this);}
 replaceChildren(...ns){raw(this).children=[];this.append(...ns);}
 getAttribute(k){return raw(this).attrs[String(k).toLowerCase()]??null;}hasAttribute(k){return this.getAttribute(k)!==null;}
 setAttribute(k,v){k=String(k).toLowerCase();v=String(v);if(k.length>64||v.length>4096||Object.keys(raw(this).attrs).length>=32&&!this.hasAttribute(k))throw Error('PSP attribute limit');raw(this).attrs[k]=v;}
 removeAttribute(k){delete raw(this).attrs[String(k).toLowerCase()];}
 get id(){return this.getAttribute('id')||'';}set id(v){this.setAttribute('id',v);}get className(){return this.getAttribute('class')||'';}set className(v){this.setAttribute('class',v);}
 get classList(){const n=this;const list=()=>n.className.split(/\s+/).filter(Boolean);return {contains:v=>list().includes(v),add(...v){n.className=[...new Set([...list(),...v])].join(' ');},remove(...v){n.className=list().filter(x=>!v.includes(x)).join(' ');},toggle(v,force){const has=this.contains(v),add=force??!has;add?this.add(v):this.remove(v);return add;}};}
 get style(){const n=this,key=k=>String(k).replace(/[A-Z]/g,c=>'-'+c.toLowerCase());const values=()=>Object.fromEntries((n.getAttribute('style')||'').split(';').map(s=>s.split(/:(.*)/s).slice(0,2).map(x=>x.trim())).filter(a=>a.length===2));return new Proxy({}, {get(_,k){if(k==='cssText')return n.getAttribute('style')||'';if(k==='setProperty')return (p,v)=>{const a=values();a[key(p)]=String(v);n.setAttribute('style',Object.entries(a).map(([p,v])=>p+':'+v).join(';'));};if(k==='getPropertyValue')return p=>values()[key(p)]||'';return values()[key(k)]||'';},set(_,k,v){if(k==='cssText')n.setAttribute('style',v);else{const a=values();a[key(k)]=String(v);n.setAttribute('style',Object.entries(a).map(([p,v])=>p+':'+v).join(';'));}return true;}});}
 get href(){return this.getAttribute('href')?__resolve(this.getAttribute('href')):'';}set href(v){this.setAttribute('href',v);}
 querySelectorAll(s){return descendants(this._id).map(wrap).filter(n=>n.nodeType===1&&matches(n,String(s)));}querySelector(s){return this.querySelectorAll(s)[0]||null;}
 matches(s){return matches(this,String(s));}closest(s){for(let n=this;n;n=n.parentNode)if(n.matches(s))return n;return null;}
 addEventListener(t,fn){if(typeof fn!=='function')return;const key=this._id+':'+t;const a=listeners.get(key)||[];if(a.length>=64)throw Error('Listener limit');a.push(fn);listeners.set(key,a);}
 removeEventListener(t,fn){const k=this._id+':'+t;listeners.set(k,(listeners.get(k)||[]).filter(f=>f!==fn));}
 dispatchEvent(e){e.target=this;for(const f of listeners.get(this._id+':'+e.type)||[])f.call(this,e);const f=this['on'+e.type];if(typeof f==='function')f.call(this,e);return !e.defaultPrevented;}
}
const document=wrap(0);Object.assign(document,{createElement:t=>create(String(t).toLowerCase()),createTextNode:t=>create('#text',String(t)),getElementById:id=>descendants(0).map(wrap).find(n=>n.id===String(id))||null,getElementsByTagName:t=>document.querySelectorAll(t),getElementsByClassName:c=>document.querySelectorAll('.'+c),write:(...s)=>fragment(document.body||document,s.join('')),writeln:(...s)=>fragment(document.body||document,s.join('')+'\n'),readyState:'loading'});
Object.defineProperties(document,{body:{get:()=>document.querySelector('body')||document},head:{get:()=>document.querySelector('head')},title:{get:()=>document.querySelector('title')?.textContent||'',set:v=>{let t=document.querySelector('title');if(!t){t=create('title');(document.head||document).appendChild(t);}t.textContent=v;}},URL:{value:__url}});
class Event {constructor(type){this.type=type;this.defaultPrevented=false;}preventDefault(){this.defaultPrevented=true;}}
Object.assign(globalThis,{document,Node,Element:Node,HTMLElement:Node,Event,window:globalThis,self:globalThis,navigator:{userAgent:'Mozilla/5.0 (PlayStation Portable; Mobile) ARKBrowser/0.3',platform:'PSP',language:'en'},location:Object.freeze({href:__url,toString:()=>__url}),console:{log(){},warn(){},error(){},info(){}},alert(){}});
globalThis.addEventListener=(t,f)=>document.addEventListener(t,f);globalThis.removeEventListener=(t,f)=>document.removeEventListener(t,f);
globalThis.fetch=async url=>{const body=__fetch(String(url));return {ok:true,status:200,url:__resolve(String(url)),text:async()=>body,json:async()=>JSON.parse(body)};};
let timerID=0;const timers=new Map();globalThis.setTimeout=(f,ms,...a)=>{if(typeof f!=='function'||timers.size>=64)throw Error('Timer unsupported/limit');const id=++timerID;timers.set(id,()=>f(...a));return id;};globalThis.clearTimeout=id=>timers.delete(id);
/* One-shot timers run once at the end of loading; delays are not emulated. */
globalThis.__finish=()=>{document.readyState='interactive';document.dispatchEvent(new Event('DOMContentLoaded'));document.readyState='complete';document.dispatchEvent(new Event('load'));if(typeof globalThis.onload==='function')globalThis.onload(new Event('load'));const pending=[...timers.values()];timers.clear();for(const f of pending)f();};
globalThis.__snapshot=()=>JSON.stringify(nodes);
})();
