/* Progressive enhancements: all page content remains available without JS. */
(() => {
  const tabList = document.querySelector('.install-tabs');
  const tabs = [...tabList.querySelectorAll('button')];
  const copyStatus = document.querySelector('#copy-status');

  function selectTab(selected, focus = false) {
    tabs.forEach(tab => {
      const active = tab === selected;
      tab.setAttribute('aria-selected', String(active));
      tab.tabIndex = active ? 0 : -1;
      document.getElementById(tab.dataset.panel).hidden = !active;
    });
    copyStatus.textContent = '';
    if (focus) selected.focus();
  }
  tabList.setAttribute('role', 'tablist');
  tabs.forEach((tab, index) => {
    const panel = document.getElementById(tab.dataset.panel);
    tab.setAttribute('role', 'tab');
    tab.setAttribute('aria-controls', panel.id);
    panel.setAttribute('role', 'tabpanel');
    panel.setAttribute('aria-labelledby', tab.id);
    panel.tabIndex = 0;
    tab.addEventListener('click', () => selectTab(tab));
    tab.addEventListener('keydown', event => {
      let next;
      if (event.key === 'ArrowRight') next = (index + 1) % tabs.length;
      if (event.key === 'ArrowLeft') next = (index - 1 + tabs.length) % tabs.length;
      if (event.key === 'Home') next = 0;
      if (event.key === 'End') next = tabs.length - 1;
      if (next === undefined) return;
      event.preventDefault();
      selectTab(tabs[next], true);
    });
  });
  selectTab(tabs[0]);
  tabList.hidden = false;

  document.querySelectorAll('.copy-button').forEach(button => {
    button.hidden = false;
    button.addEventListener('click', async () => {
      const code = document.getElementById(button.dataset.copy);
      try {
        await navigator.clipboard.writeText(code.textContent);
        copyStatus.textContent = 'Commands copied to clipboard.';
      } catch {
        // file:// and restricted clipboard contexts still get selectable commands.
        const selection = window.getSelection();
        const range = document.createRange();
        range.selectNodeContents(code);
        selection.removeAllRanges();
        selection.addRange(range);
        copyStatus.textContent = 'Commands selected. Press Ctrl+C or ⌘C to copy.';
      }
    });
  });
})();
