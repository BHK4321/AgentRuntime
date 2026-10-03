const assert = require('node:assert/strict');
const test = require('node:test');

class Node {
  constructor(tag, value = '') {
    this.tag = tag;
    this.value = value;
    this.children = [];
    this.style = {};
  }
  append(...children) { this.children.push(...children); }
  set textContent(value) { this.children = [new Node('#text', String(value))]; }
  get textContent() { return this.tag === '#text' ? this.value : this.children.map(child => child.textContent).join(''); }
}

global.document = {
  createElement: tag => new Node(tag),
  createTextNode: value => new Node('#text', value),
};
global.window = {location: new URL('http://127.0.0.1:8080/')};
require('../chat_service/static/markdown.js');

function allNodes(root) {
  return [root, ...root.children.flatMap(allNodes)];
}

test('renders the headings, tables, lists, and code blocks in an assistant reply', () => {
  const reply = window.renderMarkdown('## Z-array\n\n| i | Z[i] |\n|---|---:|\n| 1 | 7 |\n\n- First\n- Second\n\n```python\nprint("ok")\n```');
  const nodes = allNodes(reply);
  assert.ok(nodes.some(node => node.tag === 'h2' && node.textContent === 'Z-array'));
  assert.ok(nodes.some(node => node.tag === 'table'));
  assert.ok(nodes.some(node => node.tag === 'td' && node.textContent === '7'));
  assert.ok(nodes.some(node => node.tag === 'ul' && node.children.length === 2));
  assert.ok(nodes.some(node => node.tag === 'pre' && node.textContent === 'print("ok")'));
});

test('treats raw HTML and unsafe links as text', () => {
  const reply = window.renderMarkdown('<img src=x onerror=alert(1)> [bad](javascript:alert) [good](https://example.com)');
  const nodes = allNodes(reply);
  assert.equal(nodes.filter(node => node.tag === 'img').length, 0);
  assert.equal(nodes.filter(node => node.tag === 'a').length, 1);
  assert.equal(nodes.find(node => node.tag === 'a').href, 'https://example.com/');
  assert.match(reply.textContent, /<img src=x onerror=alert\(1\)>/);
});
