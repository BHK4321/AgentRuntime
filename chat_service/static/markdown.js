// Render the Markdown used in chat replies with DOM nodes, never raw HTML.
(function () {
  'use strict';

  function appendInline(parent, source) {
    let index = 0;
    let plain = '';
    const flush = () => {
      if (plain) parent.append(document.createTextNode(plain));
      plain = '';
    };
    while (index < source.length) {
      const rest = source.slice(index);
      if (rest[0] === '\\' && rest.length > 1) {
        plain += rest[1];
        index += 2;
        continue;
      }
      if (rest[0] === '`') {
        const ticks = /^`+/.exec(rest)[0];
        const end = source.indexOf(ticks, index + ticks.length);
        if (end !== -1) {
          flush();
          const code = document.createElement('code');
          code.textContent = source.slice(index + ticks.length, end);
          parent.append(code);
          index = end + ticks.length;
          continue;
        }
      }
      if (rest[0] === '[') {
        const link = /^\[([^\]]+)\]\(([^\s)]+)(?:\s+"[^"]*")?\)/.exec(rest);
        if (link) {
          try {
            const url = new URL(link[2], window.location.href);
            if (['http:', 'https:', 'mailto:'].includes(url.protocol)) {
              flush();
              const anchor = document.createElement('a');
              anchor.href = url.href;
              anchor.rel = 'noopener noreferrer';
              if (url.origin !== window.location.origin) anchor.target = '_blank';
              appendInline(anchor, link[1]);
              parent.append(anchor);
              index += link[0].length;
              continue;
            }
          } catch (_) { /* Show an invalid link as text. */ }
        }
      }
      let formatted = false;
      for (const [marker, tag] of [['**', 'strong'], ['__', 'strong'], ['~~', 's'], ['*', 'em'], ['_', 'em']]) {
        if (!rest.startsWith(marker)) continue;
        const end = source.indexOf(marker, index + marker.length);
        if (end <= index + marker.length) continue;
        flush();
        const element = document.createElement(tag);
        appendInline(element, source.slice(index + marker.length, end));
        parent.append(element);
        index = end + marker.length;
        formatted = true;
        break;
      }
      if (formatted) continue;
      plain += source[index];
      index += 1;
    }
    flush();
  }

  function tableCells(line) {
    const trimmed = line.trim().replace(/^\|/, '').replace(/\|$/, '');
    return trimmed.split(/(?<!\\)\|/).map(cell => cell.trim().replace(/\\\|/g, '|'));
  }

  function tableDelimiter(line, columns) {
    const cells = tableCells(line);
    return cells.length === columns && cells.every(cell => /^:?-{3,}:?$/.test(cell));
  }

  function listItem(line) {
    const match = /^ {0,3}([-+*]|\d+[.)])\s+(.+)$/.exec(line);
    return match ? {ordered: /^\d/.test(match[1]), number: parseInt(match[1], 10), text: match[2]} : null;
  }

  function isBlockStart(lines, index) {
    const line = lines[index];
    return /^ {0,3}(#{1,6}\s|>|`{3,}|~{3,})/.test(line) ||
      /^\s*(?:-{3,}|\*{3,}|_{3,})\s*$/.test(line) ||
      !!listItem(line) ||
      (index + 1 < lines.length && line.includes('|') && tableDelimiter(lines[index + 1], tableCells(line).length));
  }

  function renderMarkdown(source) {
    const root = document.createElement('div');
    root.className = 'markdown';
    const lines = String(source ?? '').replace(/\r\n?/g, '\n').split('\n');
    let index = 0;
    while (index < lines.length) {
      const line = lines[index];
      if (!line.trim()) { index += 1; continue; }

      const fence = /^ {0,3}(`{3,}|~{3,})(.*)$/.exec(line);
      if (fence) {
        const body = [];
        index += 1;
        while (index < lines.length && !new RegExp('^ {0,3}' + fence[1][0] + '{' + fence[1].length + ',}\\s*$').test(lines[index])) {
          body.push(lines[index++]);
        }
        if (index < lines.length) index += 1;
        const pre = document.createElement('pre');
        const code = document.createElement('code');
        const language = /^[a-z0-9_-]+/i.exec(fence[2].trim());
        if (language) code.className = 'language-' + language[0].toLowerCase();
        code.textContent = body.join('\n');
        pre.append(code);
        root.append(pre);
        continue;
      }

      const heading = /^ {0,3}(#{1,6})\s+(.+?)\s*#*\s*$/.exec(line);
      if (heading) {
        const element = document.createElement('h' + heading[1].length);
        appendInline(element, heading[2]);
        root.append(element);
        index += 1;
        continue;
      }

      if (/^\s*(?:-{3,}|\*{3,}|_{3,})\s*$/.test(line)) {
        root.append(document.createElement('hr'));
        index += 1;
        continue;
      }

      if (/^ {0,3}>/.test(line)) {
        const quoted = [];
        while (index < lines.length && /^ {0,3}>/.test(lines[index])) {
          quoted.push(lines[index++].replace(/^ {0,3}>\s?/, ''));
        }
        const block = document.createElement('blockquote');
        block.append(renderMarkdown(quoted.join('\n')));
        root.append(block);
        continue;
      }

      if (index + 1 < lines.length && line.includes('|') && tableDelimiter(lines[index + 1], tableCells(line).length)) {
        const headings = tableCells(line);
        const alignments = tableCells(lines[index + 1]).map(cell => cell.startsWith(':') && cell.endsWith(':') ? 'center' : cell.endsWith(':') ? 'right' : 'left');
        const wrap = document.createElement('div');
        wrap.className = 'markdown-table-wrap';
        const table = document.createElement('table');
        const thead = document.createElement('thead');
        const headerRow = document.createElement('tr');
        headings.forEach((value, column) => {
          const cell = document.createElement('th');
          cell.style.textAlign = alignments[column];
          appendInline(cell, value);
          headerRow.append(cell);
        });
        thead.append(headerRow);
        table.append(thead);
        index += 2;
        const tbody = document.createElement('tbody');
        while (index < lines.length && lines[index].trim() && lines[index].includes('|')) {
          const row = document.createElement('tr');
          const cells = tableCells(lines[index++]);
          headings.forEach((_, column) => {
            const cell = document.createElement('td');
            cell.style.textAlign = alignments[column];
            appendInline(cell, cells[column] || '');
            row.append(cell);
          });
          tbody.append(row);
        }
        table.append(tbody);
        wrap.append(table);
        root.append(wrap);
        continue;
      }

      const firstItem = listItem(line);
      if (firstItem) {
        const list = document.createElement(firstItem.ordered ? 'ol' : 'ul');
        if (firstItem.ordered && firstItem.number > 1) list.start = firstItem.number;
        while (index < lines.length) {
          const item = listItem(lines[index]);
          if (!item || item.ordered !== firstItem.ordered) break;
          const element = document.createElement('li');
          appendInline(element, item.text);
          list.append(element);
          index += 1;
        }
        root.append(list);
        continue;
      }

      const paragraph = [line.trim()];
      index += 1;
      while (index < lines.length && lines[index].trim() && !isBlockStart(lines, index)) {
        paragraph.push(lines[index++].trim());
      }
      const element = document.createElement('p');
      appendInline(element, paragraph.join(' '));
      root.append(element);
    }
    return root;
  }

  window.renderMarkdown = renderMarkdown;
})();
