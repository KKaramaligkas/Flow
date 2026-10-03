/* PSP reader DOM. No filesystem, process, or native module bindings. */
(() => {
'use strict';
const nodes=__seed, wrappers=new Map(), listeners=new Map(), parents=nodes.map(()=>-1);
delete globalThis.__seed;
nodes.forEach((n,i)=>{for(const c of n.children)parents[c]=i;});
/* Scripts may add 2048 nodes to the page's, within the browser's 16,384. */
const limit=Math.min(nodes.length+2048,16384);
const raw=n=>nodes[n._id], wrap=id=>id==null||id<0?null:wrappers.get(id)||new Node(id);
function create(tag,text=''){if(nodes.length>=limit)throw Error('PSP DOM node limit');const id=nodes.length;nodes.push({tag,text,attrs:{},children:[]});parents.push(-1);return wrap(id);}
/* Whether scripts changed the page: if not, the browser keeps its own copy. */
let changed=false;
function empty(id){for(const c of nodes[id].children)parents[c]=-1;nodes[id].children=[];changed=true;}
/* Selectors: a tag, #id, .class, [attribute] and pseudo-classes, joined by
   descendant, >, + and ~ combinators. Each selector is parsed once. */
const parsed=new Map(), bad=()=>new SyntaxError('Unsupported selector');let scope=-1;
function closing(s,i){const open=s[i],close=open==='['?']':')';let depth=0,quote='';for(;i<s.length;i++){const c=s[i];if(quote){if(c==='\\')i++;else if(c===quote)quote='';}else if(c==='"'||c==="'")quote=c;else if(c===open)depth++;else if(c===close&&!--depth)return i;}throw bad();}
function ident(s,i){const m=s.slice(i).match(/^(?:\\.|[\w\u00a0-\uffff-])+/);if(!m)throw bad();return [m[0].replace(/\\(.)/g,'$1'),i+m[0].length];}
function complex(s){
 const seq=[];let i=0,v;
 while(i<s.length){
  let comb='';while(/\s/.test(s[i]||'')){i++;comb=' ';}
  if(s[i]&&'>+~'.includes(s[i])){comb=s[i++];while(/\s/.test(s[i]||''))i++;}
  if(i>=s.length)break;
  if(seq.length){if(!comb)throw bad();seq.push(comb);}else if(comb.trim())throw bad();
  const c={tag:'',id:null,classes:[],attrs:[],pseudos:[],never:false},from=i;
  if(s[i]==='*')i++;else if(/[\w\\-]/.test(s[i]))[v,i]=ident(s,i),c.tag=v.toLowerCase();
  for(;;){
   if(s[i]==='#')[c.id,i]=ident(s,i+1);
   else if(s[i]==='.'){[v,i]=ident(s,i+1);c.classes.push(v);}
   else if(s[i]==='['){const end=closing(s,i),m=s.slice(i+1,end).match(/^\s*([^\s~|^$*=\]]+)\s*(?:([~|^$*]?=)\s*(?:"((?:\\.|[^"])*)"|'((?:\\.|[^'])*)'|([^\s\]]+))\s*([iIsS])?)?\s*$/);if(!m)throw bad();c.attrs.push([m[1].toLowerCase(),m[2],(m[3]??m[4]??m[5]??'').replace(/\\(.)/g,'$1'),/i/i.test(m[6]||'')]);i=end+1;}
   else if(s[i]===':'){if(s[i+1]===':'){[v,i]=ident(s,i+2);if(s[i]==='(')i=closing(s,i)+1;c.never=true;continue;}[v,i]=ident(s,i+1);let arg=null;if(s[i]==='('){const end=closing(s,i);arg=s.slice(i+1,end);i=end+1;}c.pseudos.push([v.toLowerCase(),arg]);}
   else break;}
  if(i===from||(i<s.length&&!/[\s>+~]/.test(s[i])))throw bad();
  seq.push(c);}
 if(!seq.length||typeof seq[seq.length-1]==='string')throw bad();
 return seq;}
function list(text){let l=parsed.get(text);if(l)return l;l=[];let start=0,depth=0,quote='';
 for(let i=0;i<=text.length;i++){const c=text[i];if(quote){if(c==='\\')i++;else if(c===quote)quote='';}else if(c==='"'||c==="'")quote=c;else if(c==='('||c==='[')depth++;else if(c===')'||c===']')depth--;else if(i===text.length||(c===','&&!depth)){l.push(complex(text.slice(start,i).trim()));start=i+1;}}
 if(parsed.size>=256)parsed.clear();parsed.set(text,l);return l;}
const elements=i=>{const p=parents[i];return p<0?[i]:nodes[p].children.filter(k=>nodes[k].tag[0]!=='#');};
function nth(k,arg){const m=String(arg).replace(/\s+/g,'').toLowerCase().match(/^(?:(odd)|(even)|([+-]?\d*)n([+-]\d+)?|([+-]?\d+))$/);if(!m)return false;if(m[1])return k%2===1;if(m[2])return k%2===0;if(m[5]!==undefined)return k===+m[5];const a=m[3]===''||m[3]==='+'?1:m[3]==='-'?-1:+m[3],b=+(m[4]||0);return a?(k-b)/a>=0&&(k-b)%a===0:k===b;}
function pseudo(i,p,arg){const n=nodes[i],sib=()=>elements(i),type=()=>sib().filter(k=>nodes[k].tag===n.tag);
 switch(p){
  case 'not':return !matching(i,list(arg??''));
  case 'is':case 'where':case 'matches':case '-webkit-any':return matching(i,list(arg??''));
  case 'first-child':return sib()[0]===i;case 'last-child':return sib().at(-1)===i;case 'only-child':return sib().length===1;
  case 'first-of-type':return type()[0]===i;case 'last-of-type':return type().at(-1)===i;case 'only-of-type':return type().length===1;
  case 'nth-child':return nth(sib().indexOf(i)+1,arg);case 'nth-last-child':{const s=sib();return nth(s.length-s.indexOf(i),arg);}
  case 'nth-of-type':return nth(type().indexOf(i)+1,arg);case 'nth-last-of-type':{const s=type();return nth(s.length-s.indexOf(i),arg);}
  case 'empty':return !n.children.some(k=>nodes[k].tag!=='#comment'&&(nodes[k].tag!=='#text'||nodes[k].text));
  case 'root':return n.tag==='html';case 'scope':return scope>0?i===scope:n.tag==='html';
  case 'checked':return 'checked' in n.attrs||(n.tag==='option'&&'selected' in n.attrs);
  case 'disabled':return 'disabled' in n.attrs;case 'enabled':return /^(input|button|select|textarea|option|optgroup|fieldset)$/.test(n.tag)&&!('disabled' in n.attrs);
  case 'link':case 'any-link':return /^(a|area)$/.test(n.tag)&&'href' in n.attrs;
  default:return false;}}   /* :hover, :focus...: states of a page nobody has touched */
/* Whether space-separated `list` holds `word`. */
function word(list,w){for(let at=list.indexOf(w);at>=0;at=list.indexOf(w,at+1)){const before=at?list.charCodeAt(at-1):32,after=at+w.length<list.length?list.charCodeAt(at+w.length):32;if(before<=32&&after<=32)return true;}return false;}
/* The hot path of every query: plain loops, no iterators or closures. */
function compound(i,c){const n=nodes[i],t=n.tag;if(t.charCodeAt(0)===35||c.never||(c.tag&&c.tag!==t))return false;const a=n.attrs;if(c.id!==null&&a.id!==c.id)return false;
 const cl=c.classes;if(cl.length){const own=a.class;if(!own)return false;for(let j=0;j<cl.length;j++)if(!word(own,cl[j]))return false;}
 const at=c.attrs;for(let j=0;j<at.length;j++){const k=at[j][0],op=at[j][1];let x=a[k];if(x===undefined)return false;if(!op)continue;let y=at[j][2];if(at[j][3]){x=x.toLowerCase();y=y.toLowerCase();}
  if(!(op==='='?x===y:op==='~='?word(x,y):op==='|='?x===y||x.startsWith(y+'-'):y!==''&&(op==='^='?x.startsWith(y):op==='$='?x.endsWith(y):x.includes(y))))return false;}
 const ps=c.pseudos;for(let j=0;j<ps.length;j++)if(!pseudo(i,ps[j][0],ps[j][1]))return false;
 return true;}
function step(i,seq,k){if(!compound(i,seq[k]))return false;if(!k)return true;const comb=seq[k-1];
 if(comb==='>')return parents[i]>=0&&step(parents[i],seq,k-2);
 if(comb===' '){for(let p=parents[i];p>=0;p=parents[p])if(step(p,seq,k-2))return true;return false;}
 const s=elements(i),at=s.indexOf(i);if(comb==='+')return at>0&&step(s[at-1],seq,k-2);
 for(let j=at-1;j>=0;j--)if(step(s[j],seq,k-2))return true;return false;}
function matching(i,l){for(let k=0;k<l.length;k++)if(step(i,l[k],l[k].length-1))return true;return false;}
/* The elements under `id` that match selector list `l`, in document order: all of them, or the first. */
function select(id,l,first){const out=[],stack=[];let c=nodes[id].children;for(let k=c.length-1;k>=0;k--)stack.push(c[k]);scope=id;
 /* A single selector's last tag, id or class rules out most nodes at a glance. */
 const last=l.length===1?l[0][l[0].length-1]:null,wantTag=last?.tag||'',wantId=last?last.id:null,wantClass=last?.classes[0]||'';
 try{for(let steps=0;stack.length;){const i=stack.pop();if(++steps>65536)throw Error('DOM cycle');const n=nodes[i];c=n.children;for(let k=c.length-1;k>=0;k--)stack.push(c[k]);
  if(n.tag.charCodeAt(0)===35||(wantTag&&n.tag!==wantTag)||(wantId!==null&&n.attrs.id!==wantId)||(wantClass&&!(n.attrs.class?.includes(wantClass))))continue;
  if(matching(i,l)){out.push(wrap(i));if(first)break;}}}finally{scope=-1;}
 return out;}
function decode(s){return s.replace(/&(#x[\da-f]+|#\d+|amp|lt|gt|quot|apos|nbsp);/gi,(_,n)=>n[0]==='#'?String.fromCodePoint(Math.min(0x10ffff,parseInt(n.slice(n[1].toLowerCase()==='x'?2:1),n[1].toLowerCase()==='x'?16:10))):({amp:'&',lt:'<',gt:'>',quot:'"',apos:"'",nbsp:' '})[n.toLowerCase()]);}
function fragment(parent,html){if(html.length>262144)throw Error('PSP HTML limit');const stack=[parent];const tokens=html.match(/<!--[\s\S]*?-->|<[^>]*>|<[a-zA-Z\/!?][^>]*$|[^<]+|</g)||[];for(const token of tokens){if(token.startsWith('<!--')||(token[0]==='<'&&!token.endsWith('>')))continue;if(token.startsWith('</')){if(stack.length>1)stack.pop();continue;}if(token[0]==='<'){const m=token.match(/^<([\w-]+)/);if(!m)continue;const n=create(m[1].toLowerCase());for(const a of token.slice(m[0].length).matchAll(/([\w:-]+)(?:\s*=\s*(?:"([^"]*)"|'([^']*)'|([^\s>]+)))?/g))n.setAttribute(a[1],decode(a[2]??a[3]??a[4]??''));stack.at(-1).appendChild(n);if(!/^(br|hr|img|input|meta|link|base)$/.test(raw(n).tag)&&!token.endsWith('/>')){if(stack.length>=64)throw Error('DOM depth');stack.push(n);}}else stack.at(-1).appendChild(create('#text',decode(token)));}}
const node=v=>v instanceof Node?v:create('#text',String(v));
class Node {
 constructor(id){Object.defineProperty(this,'_id',{value:id});wrappers.set(id,this);}
 get nodeType(){const t=raw(this).tag;return t==='#document'?9:t==='#text'?3:t==='#comment'?8:t==='#fragment'?11:1;}
 get nodeName(){const t=raw(this).tag;return t[0]==='#'?t:t.toUpperCase();}get tagName(){return this.nodeName;}get localName(){return raw(this).tag;}
 get nodeValue(){return this.nodeType===3||this.nodeType===8?raw(this).text:null;}set nodeValue(v){if(this.nodeType===3||this.nodeType===8){raw(this).text=String(v);changed=true;}}
 get data(){return this.nodeValue;}set data(v){this.nodeValue=v;}
 get ownerDocument(){return this._id?document:null;}
 get childNodes(){return raw(this).children.map(wrap);}get children(){return this.childNodes.filter(n=>n.nodeType===1);}get firstChild(){return wrap(raw(this).children[0]);}get lastChild(){return wrap(raw(this).children.at(-1));}
 get firstElementChild(){return this.children[0]||null;}get lastElementChild(){return this.children.at(-1)||null;}get childElementCount(){return this.children.length;}hasChildNodes(){return raw(this).children.length>0;}
 get parentNode(){return wrap(parents[this._id]);}get parentElement(){const p=parents[this._id];return p>0&&nodes[p].tag[0]!=='#'?wrap(p):null;}
 get nextSibling(){const p=parents[this._id];if(p<0)return null;const c=nodes[p].children;return wrap(c[c.indexOf(this._id)+1]);}
 get previousSibling(){const p=parents[this._id];if(p<0)return null;const c=nodes[p].children;return wrap(c[c.indexOf(this._id)-1]);}
 get nextElementSibling(){let n=this.nextSibling;while(n&&n.nodeType!==1)n=n.nextSibling;return n;}
 get previousElementSibling(){let n=this.previousSibling;while(n&&n.nodeType!==1)n=n.previousSibling;return n;}
 get isConnected(){return connected(this._id);}
 contains(n){for(let i=n instanceof Node?n._id:-1;i>=0;i=parents[i])if(i===this._id)return true;return false;}
 get textContent(){if(this.nodeType===3||this.nodeType===8||raw(this).tag==='style')return raw(this).text;return this.childNodes.map(n=>n.nodeType===8?'':n.textContent).join('');}
 set textContent(v){v=String(v);if(v.length>262144)throw Error('PSP text limit');if(this.nodeType===3||this.nodeType===8||raw(this).tag==='style'){raw(this).text=v;changed=true;}else{empty(this._id);if(v)this.appendChild(create('#text',v));}}
 get innerText(){return this.textContent;}set innerText(v){this.textContent=v;}
 get innerHTML(){return this.childNodes.map(n=>n.nodeType===3?n.textContent:n.nodeType===1?`<${raw(n).tag}>${n.innerHTML}</${raw(n).tag}>`:'').join('');}
 set innerHTML(v){empty(this._id);fragment(this,String(v));}
 get outerHTML(){return `<${raw(this).tag}>${this.innerHTML}</${raw(this).tag}>`;}
 insertAdjacentHTML(where,html){const t=create('div');fragment(t,String(html));const kids=raw(t).children.map(wrap),p=this.parentNode;where=String(where).toLowerCase();
  const at=where==='beforebegin'?this:where==='afterbegin'?this.firstChild:where==='afterend'?this.nextSibling:null;
  for(const k of kids)(where==='beforebegin'||where==='afterend'?p:this)?.insertBefore(k,at);}
 appendChild(n){return this.insertBefore(n,null);}append(...ns){for(const n of ns)this.appendChild(node(n));}
 prepend(...ns){const first=this.firstChild;for(const n of ns)this.insertBefore(node(n),first);}
 before(...ns){const p=this.parentNode;for(const n of ns)p?.insertBefore(node(n),this);}after(...ns){const p=this.parentNode,next=this.nextSibling;for(const n of ns)p?.insertBefore(node(n),next);}
 replaceWith(...ns){const p=this.parentNode,next=this.nextSibling;if(!p)return;this.remove();for(const n of ns)p.insertBefore(node(n),next);}
 insertBefore(n,ref){if(!(n instanceof Node)||n._id===0||n===this)throw Error('Invalid DOM insertion');
  if(raw(n).tag==='#fragment'){for(const c of raw(n).children.slice())this.insertBefore(wrap(c),ref);return n;}
  for(let p=this._id,d=0;p>=0;p=parents[p]){if(p===n._id||++d>64)throw Error('DOM cycle');}
  let at=ref==null?raw(this).children.length:raw(this).children.indexOf(ref._id);if(at<0)throw Error('Not a child');n.remove();if(ref)at=raw(this).children.indexOf(ref._id);raw(this).children.splice(at,0,n._id);parents[n._id]=this._id;changed=true;return n;}
 removeChild(n){const i=raw(this).children.indexOf(n._id);if(i<0)throw Error('Not a child');raw(this).children.splice(i,1);parents[n._id]=-1;changed=true;return n;}remove(){this.parentNode?.removeChild(this);}
 replaceChild(n,old){this.insertBefore(n,old);return this.removeChild(old);}
 replaceChildren(...ns){empty(this._id);this.append(...ns);}
 cloneNode(deep){const r=raw(this),c=create(r.tag,r.text);Object.assign(raw(c).attrs,r.attrs);if(deep)for(const k of r.children)c.appendChild(wrap(k).cloneNode(true));return c;}
 getAttribute(k){return raw(this).attrs[String(k).toLowerCase()]??null;}hasAttribute(k){return this.getAttribute(k)!==null;}
 setAttribute(k,v){k=String(k).toLowerCase();v=String(v);if(k.length>64||v.length>4096||Object.keys(raw(this).attrs).length>=32&&!this.hasAttribute(k))throw Error('PSP attribute limit');raw(this).attrs[k]=v;changed=true;}
 removeAttribute(k){delete raw(this).attrs[String(k).toLowerCase()];changed=true;}toggleAttribute(k,force){const on=force??!this.hasAttribute(k);if(on)this.setAttribute(k,'');else this.removeAttribute(k);return on;}
 getAttributeNames(){return Object.keys(raw(this).attrs);}
 get attributes(){return Object.entries(raw(this).attrs).map(([name,value])=>({name,value}));}
 get id(){return this.getAttribute('id')||'';}set id(v){this.setAttribute('id',v);}get className(){return this.getAttribute('class')||'';}set className(v){this.setAttribute('class',v);}
 get classList(){const n=this;const list=()=>n.className.split(/\s+/).filter(Boolean);return {contains:v=>list().includes(v),add(...v){n.className=[...new Set([...list(),...v])].join(' ');},remove(...v){n.className=list().filter(x=>!v.includes(x)).join(' ');},toggle(v,force){const has=this.contains(v),add=force??!has;add?this.add(v):this.remove(v);return add;},replace(a,b){if(!this.contains(a))return false;n.className=list().map(x=>x===a?b:x).join(' ');return true;},item:i=>list()[i]??null,get length(){return list().length;},get value(){return n.className;},forEach:f=>list().forEach(f),[Symbol.iterator]:()=>list()[Symbol.iterator]()};}
 get dataset(){const n=this,key=k=>'data-'+String(k).replace(/[A-Z]/g,c=>'-'+c.toLowerCase());return new Proxy({},{get:(_,k)=>typeof k==='string'?n.getAttribute(key(k))??undefined:undefined,set:(_,k,v)=>(n.setAttribute(key(k),v),true),has:(_,k)=>n.hasAttribute(key(k)),deleteProperty:(_,k)=>(n.removeAttribute(key(k)),true)});}
 get style(){const n=this,key=k=>String(k).replace(/[A-Z]/g,c=>'-'+c.toLowerCase());const values=()=>Object.fromEntries((n.getAttribute('style')||'').split(';').map(s=>s.split(/:(.*)/s).slice(0,2).map(x=>x.trim())).filter(a=>a.length===2));const write=a=>n.setAttribute('style',Object.entries(a).filter(([,v])=>v!=='').map(([p,v])=>p+':'+v).join(';'));return new Proxy({}, {get(_,k){if(k==='cssText')return n.getAttribute('style')||'';if(k==='setProperty')return (p,v)=>{const a=values();a[key(p)]=String(v??'');write(a);};if(k==='removeProperty')return p=>{const a=values(),old=a[key(p)]||'';delete a[key(p)];write(a);return old;};if(k==='getPropertyValue')return p=>values()[key(p)]||'';return typeof k==='string'?values()[key(k)]||'':undefined;},set(_,k,v){if(k==='cssText')n.setAttribute('style',v);else{const a=values();a[key(k)]=String(v??'');write(a);}return true;}});}
 get href(){return this.getAttribute('href')?__resolve(this.getAttribute('href')):'';}set href(v){this.setAttribute('href',v);}
 get value(){return this.getAttribute('value')??(raw(this).tag==='textarea'?this.textContent:'');}set value(v){if(raw(this).tag==='textarea')this.textContent=v;else this.setAttribute('value',v);}
 get checked(){return this.hasAttribute('checked');}set checked(v){this.toggleAttribute('checked',!!v);}
 get disabled(){return this.hasAttribute('disabled');}set disabled(v){this.toggleAttribute('disabled',!!v);}
 get hidden(){return this.hasAttribute('hidden');}set hidden(v){this.toggleAttribute('hidden',!!v);}
 get src(){return this.getAttribute('src')?__resolve(this.getAttribute('src')):'';}set src(v){this.setAttribute('src',v);}
 get name(){return this.getAttribute('name')||'';}set name(v){this.setAttribute('name',v);}get type(){return this.getAttribute('type')||'';}set type(v){this.setAttribute('type',v);}
 querySelectorAll(s){return select(this._id,list(String(s)),false);}querySelector(s){return select(this._id,list(String(s)),true)[0]||null;}
 getElementsByTagName(t){return this.querySelectorAll(t);}getElementsByClassName(c){return this.querySelectorAll(String(c).trim().split(/\s+/).map(x=>'.'+x).join(''));}
 matches(s){return matching(this._id,list(String(s)));}closest(s){const l=list(String(s));for(let i=this._id;i>=0;i=parents[i])if(matching(i,l))return wrap(i);return null;}
 getBoundingClientRect(){return {x:0,y:0,top:0,left:0,right:0,bottom:0,width:0,height:0};}getClientRects(){return [];}
 get offsetParent(){return null;}focus(){}blur(){}scrollIntoView(){}scrollTo(){}click(){this.dispatchEvent(new Event('click'));}
 addEventListener(t,fn){if(typeof fn!=='function'&&typeof fn?.handleEvent!=='function')return;const key=this._id+':'+t;const a=listeners.get(key)||[];if(a.length>=64)throw Error('Listener limit');a.push(fn);listeners.set(key,a);}
 removeEventListener(t,fn){const k=this._id+':'+t;listeners.set(k,(listeners.get(k)||[]).filter(f=>f!==fn));}
 /* A listener that throws doesn't stop the others, as in a browser. */
 dispatchEvent(e){e.target=e.currentTarget=this;for(const f of listeners.get(this._id+':'+e.type)||[])try{typeof f==='function'?f.call(this,e):f.handleEvent(e);}catch(err){failures++;}const f=this['on'+e.type];if(typeof f==='function')try{f.call(this,e);}catch(err){failures++;}return !e.defaultPrevented;}
}
let failures=0;
/* Sizes and positions aren't laid out here. */
for(const k of ['offsetWidth','offsetHeight','offsetTop','offsetLeft','clientWidth','clientHeight','clientTop','clientLeft','scrollWidth','scrollHeight','scrollTop','scrollLeft'])Object.defineProperty(Node.prototype,k,{get:()=>0,set(){},configurable:true});
/* <html> is a child of the document, <head> and <body> children of <html>. */
const document=wrap(0),find=tag=>{for(const i of nodes[0].children){if(nodes[i].tag===tag)return wrap(i);if(nodes[i].tag==='html')for(const k of nodes[i].children)if(nodes[k].tag===tag)return wrap(k);}return null;};
const connected=i=>{for(let d=0;i>0&&d<=64;d++)i=parents[i];return i===0;};
Object.assign(document,{createElement:t=>create(String(t).toLowerCase()),createElementNS:(ns,t)=>create(String(t).toLowerCase()),createTextNode:t=>create('#text',String(t)),createComment:t=>create('#comment',String(t)),createDocumentFragment:()=>create('#fragment'),
 getElementById:id=>{id=String(id);for(let i=1;i<nodes.length;i++)if(nodes[i].attrs.id===id&&connected(i))return wrap(i);return null;},getElementsByName:n=>document.querySelectorAll(`[name="${String(n).replace(/["\\]/g,'\\$&')}"]`),
 write:(...s)=>fragment(document.body||document,s.join('')),writeln:(...s)=>fragment(document.body||document,s.join('')+'\n'),readyState:'loading',hasFocus:()=>false,createEvent:()=>new Event('')});
Object.defineProperties(document,{body:{get:()=>find('body')||document},head:{get:()=>find('head')},documentElement:{get:()=>find('html')},scrollingElement:{get:()=>find('html')},activeElement:{get:()=>find('body')},
 title:{get:()=>document.querySelector('title')?.textContent||'',set:v=>{let t=document.querySelector('title');if(!t){t=create('title');(document.head||document).appendChild(t);}t.textContent=v;}},
 URL:{value:__url},documentURI:{value:__url},cookie:{get:()=>'',set(){}},referrer:{value:''},defaultView:{get:()=>globalThis},location:{get:()=>location},
 visibilityState:{value:'visible'},hidden:{value:false},characterSet:{value:'UTF-8'},compatMode:{value:'CSS1Compat'},contentType:{value:'text/html'}});
class Event {constructor(type,o={}){this.type=String(type);this.bubbles=!!o?.bubbles;this.cancelable=!!o?.cancelable;this.defaultPrevented=false;this.timeStamp=Date.now();}preventDefault(){this.defaultPrevented=true;}stopPropagation(){}stopImmediatePropagation(){}initEvent(type){this.type=String(type);}}
class CustomEvent extends Event {constructor(type,o={}){super(type,o);this.detail=o?.detail??null;}}
/* Addresses: the page's location, URL and URLSearchParams. */
function parts(href){const m=String(href).match(/^([a-z][\w+.-]*:)(?:\/\/(?:[^@/?#]*@)?([^/?#:]*)(?::(\d*))?)?([^?#]*)(\?[^#]*)?(#.*)?$/i);if(!m)throw new TypeError('Invalid URL');return {protocol:m[1].toLowerCase(),hostname:(m[2]||'').toLowerCase(),port:m[3]||'',pathname:m[4]||(m[2]!==undefined?'/':''),search:m[5]&&m[5]!=='?'?m[5]:'',hash:m[6]&&m[6]!=='#'?m[6]:''};}
const unescape=s=>{try{return decodeURIComponent(s.replace(/\+/g,' '));}catch{return s;}}, escape=s=>encodeURIComponent(s).replace(/%20/g,'+');
class URLSearchParams {
 constructor(q=''){this._list=[];if(typeof q==='string'){for(const p of q.replace(/^\?/,'').split('&'))if(p){const [k,v='']=p.split(/=(.*)/s);this._list.push([unescape(k),unescape(v)]);}}else if(q&&typeof q==='object')for(const [k,v] of (Symbol.iterator in q?q:Object.entries(q)))this._list.push([String(k),String(v)]);}
 get(k){return this._list.find(p=>p[0]===String(k))?.[1]??null;}getAll(k){return this._list.filter(p=>p[0]===String(k)).map(p=>p[1]);}has(k){return this._list.some(p=>p[0]===String(k));}
 set(k,v){k=String(k);v=String(v);const i=this._list.findIndex(p=>p[0]===k);if(i<0)this._list.push([k,v]);else{this._list[i][1]=v;this._list=this._list.filter((p,j)=>j<=i||p[0]!==k);}this._changed?.();}
 append(k,v){this._list.push([String(k),String(v)]);this._changed?.();}delete(k){this._list=this._list.filter(p=>p[0]!==String(k));this._changed?.();}
 get size(){return this._list.length;}forEach(f){for(const [k,v] of this._list)f(v,k,this);}entries(){return this._list.map(p=>[...p])[Symbol.iterator]();}keys(){return this._list.map(p=>p[0])[Symbol.iterator]();}values(){return this._list.map(p=>p[1])[Symbol.iterator]();}[Symbol.iterator](){return this.entries();}
 toString(){return this._list.map(([k,v])=>escape(k)+'='+escape(v)).join('&');}}
class URL {
 constructor(u,base){Object.assign(this,parts(__resolve(String(u),base===undefined?undefined:String(base))));const p=new URLSearchParams(this.search);p._changed=()=>{const s=p.toString();this.search=s?'?'+s:'';};Object.defineProperty(this,'searchParams',{value:p});}
 get host(){return this.hostname+(this.port?':'+this.port:'');}get origin(){return this.protocol+'//'+this.host;}
 get href(){return this.protocol+(this.hostname||this.protocol==='file:'?'//'+this.host:'')+this.pathname+this.search+this.hash;}set href(v){Object.assign(this,parts(__resolve(String(v))));}
 toString(){return this.href;}toJSON(){return this.href;}static canParse(u,base){try{new URL(u,base);return true;}catch{return false;}}}
const location=Object.freeze(Object.assign(parts(__url),{href:__url,origin:new URL(__url).origin,host:new URL(__url).host,toString:()=>__url,assign(){},replace(){},reload(){}}));
/* Timers run once each at the end of loading: delays aren't emulated, and
   intervals and animation frames run once. */
let timerID=0;const timers=new Map();
const later=(f,args)=>{if(typeof f!=='function'||timers.size>=64)throw Error('Timer unsupported/limit');const id=++timerID;timers.set(id,()=>f(...args));return id;},cancel=id=>{timers.delete(id);};
const storage=()=>{const m=new Map();return {get length(){return m.size;},key:i=>[...m.keys()][i]??null,getItem:k=>m.has(String(k))?m.get(String(k)):null,setItem(k,v){m.set(String(k),String(v));},removeItem(k){m.delete(String(k));},clear(){m.clear();}};};
const observer=class {observe(){}unobserve(){}disconnect(){}takeRecords(){return [];}};
const start=Date.now();
/* Same-origin GET requests, like fetch below. */
class XMLHttpRequest {
 constructor(){this.readyState=0;this.status=0;this.statusText='';this.responseText='';this.response='';this.responseType='';this.timeout=0;this.withCredentials=false;}
 open(method,url){this._method=String(method).toUpperCase();this._url=String(url);this.readyState=1;}
 setRequestHeader(){}getResponseHeader(){return null;}getAllResponseHeaders(){return '';}overrideMimeType(){}abort(){}
 addEventListener(t,f){if(typeof f==='function')this['on'+t]=f;}removeEventListener(){}
 send(){Promise.resolve().then(()=>{let ok=false;if(this._method==='GET'){try{this.responseText=__fetch(this._url);ok=true;}catch{}}
  this.readyState=4;this.status=ok?200:0;this.statusText=ok?'OK':'';this.response=this.responseType==='json'?(()=>{try{return JSON.parse(this.responseText);}catch{return null;}})():this.responseText;
  for(const t of ['readystatechange',ok?'load':'error','loadend'])if(typeof this['on'+t]==='function')this['on'+t](new Event(t));});}}
Object.assign(XMLHttpRequest,{UNSENT:0,OPENED:1,HEADERS_RECEIVED:2,LOADING:3,DONE:4});
Object.assign(globalThis,{document,Node,Element:Node,HTMLElement:Node,SVGElement:Node,Text:Node,Comment:Node,DocumentFragment:Node,Document:Node,HTMLDocument:Node,
 HTMLAnchorElement:Node,HTMLButtonElement:Node,HTMLDivElement:Node,HTMLFormElement:Node,HTMLImageElement:Node,HTMLInputElement:Node,HTMLScriptElement:Node,HTMLSelectElement:Node,HTMLSpanElement:Node,HTMLTemplateElement:Node,HTMLTextAreaElement:Node,
 Event,CustomEvent,URL,URLSearchParams,XMLHttpRequest,window:globalThis,self:globalThis,top:globalThis,parent:globalThis,frames:globalThis,
 navigator:{userAgent:'Mozilla/5.0 (PlayStation Portable; Mobile) ARKBrowser/0.3',platform:'PSP',language:'en',languages:['en'],onLine:true,cookieEnabled:false,maxTouchPoints:0,sendBeacon:()=>false},
 location,history:{length:1,state:null,scrollRestoration:'auto',pushState(){},replaceState(){},back(){},forward(){},go(){}},
 screen:{width:480,height:272,availWidth:480,availHeight:272,colorDepth:32,pixelDepth:32},
 innerWidth:786,innerHeight:453,outerWidth:786,outerHeight:453,devicePixelRatio:1,scrollX:0,scrollY:0,pageXOffset:0,pageYOffset:0,
 scrollTo(){},scrollBy(){},scroll(){},focus(){},blur(){},open:()=>null,close(){},print(){},postMessage(){},getSelection:()=>null,
 getComputedStyle:n=>n.style,matchMedia:q=>({matches:__media(String(q)),media:String(q),onchange:null,addListener(){},removeListener(){},addEventListener(){},removeEventListener(){}}),
 localStorage:storage(),sessionStorage:storage(),performance:{timeOrigin:start,now:()=>Date.now()-start,mark(){},measure(){},getEntriesByName:()=>[],getEntriesByType:()=>[]},
 MutationObserver:observer,IntersectionObserver:observer,ResizeObserver:observer,PerformanceObserver:observer,customElements:{define(){},get:()=>undefined,whenDefined:()=>new Promise(()=>{})},
 console:{log(){},warn(){},error(){},info(){},debug(){},trace(){},group(){},groupEnd(){},table(){}},alert(){},confirm:()=>false,prompt:()=>null,
 setTimeout:(f,ms,...a)=>later(f,a),clearTimeout:cancel,setInterval:(f,ms,...a)=>later(f,a),clearInterval:cancel,
 requestAnimationFrame:f=>later(f,[performance.now()]),cancelAnimationFrame:cancel,requestIdleCallback:f=>later(f,[{didTimeout:false,timeRemaining:()=>0}]),cancelIdleCallback:cancel,
 queueMicrotask:f=>{Promise.resolve().then(f);}});
globalThis.addEventListener=(t,f)=>document.addEventListener(t,f);globalThis.removeEventListener=(t,f)=>document.removeEventListener(t,f);globalThis.dispatchEvent=e=>document.dispatchEvent(e);
globalThis.fetch=async url=>{const body=__fetch(String(url instanceof URL?url.href:url?.url??url));return {ok:true,status:200,url:__resolve(String(url instanceof URL?url.href:url?.url??url)),headers:{get:()=>null},text:async()=>body,json:async()=>JSON.parse(body)};};
globalThis.__finish=()=>{document.readyState='interactive';document.dispatchEvent(new Event('DOMContentLoaded'));document.readyState='complete';document.dispatchEvent(new Event('load'));if(typeof globalThis.onload==='function')try{globalThis.onload(new Event('load'));}catch(e){failures++;}const pending=[...timers.values()];timers.clear();for(const f of pending)try{f();}catch(e){failures++;}if(failures)throw Error(failures+' event listeners or timers failed');};
/* The page as JSON for the browser, or '' when scripts left it as it was.
   Nothing runs afterwards, so the scripts' DOM is let go at once: the
   browser's copy of the text needs room. */
globalThis.__snapshot=()=>{const json=changed?JSON.stringify(nodes):'';nodes.length=0;parents.length=0;wrappers.clear();listeners.clear();timers.clear();parsed.clear();return json;};
})();
