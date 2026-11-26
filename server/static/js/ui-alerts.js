/**
 * Valvindo IoT — Custom Alert, Confirm, Prompt & Toast UI System
 * Zero-dependency, offline-first, XSS-safe, compatible with all pages.
 *
 * API:
 *   UIAlert.toast(message, type = 'success', title = '', duration = 4000)
 *   await UIAlert.alert(optionsOrMessage, type = 'info')
 *   await UIAlert.confirm(optionsOrMessage) -> Promise<boolean>
 *   await UIAlert.prompt(optionsOrMessage, defaultValue = '') -> Promise<string|null>
 */
(function (global) {
    'use strict';

    const THEME = {
        success: {
            accent: '#16a34a',
            softBg: '#dcfce7',
            border: '#86efac',
            icon: 'fa-check-circle',
            defaultTitle: 'Berhasil'
        },
        error: {
            accent: '#dc2626',
            softBg: '#fee2e2',
            border: '#fca5a5',
            icon: 'fa-times-circle',
            defaultTitle: 'Terjadi Kesalahan'
        },
        warning: {
            accent: '#d97706',
            softBg: '#fef3c7',
            border: '#fcd34d',
            icon: 'fa-exclamation-triangle',
            defaultTitle: 'Perhatian'
        },
        info: {
            accent: '#2563eb',
            softBg: '#dbeafe',
            border: '#93c5fd',
            icon: 'fa-info-circle',
            defaultTitle: 'Informasi'
        }
    };

    function injectStyles() {
        if (document.getElementById('ui-alert-styles')) return;
        const style = document.createElement('style');
        style.id = 'ui-alert-styles';
        style.textContent = `
            .uia-toast-stack {
                position: fixed;
                top: 20px;
                right: 20px;
                z-index: 10000;
                display: flex;
                flex-direction: column;
                gap: 10px;
                max-width: 380px;
                width: calc(100vw - 32px);
                pointer-events: none;
            }
            .uia-toast {
                pointer-events: auto;
                position: relative;
                overflow: hidden;
                display: flex;
                align-items: flex-start;
                gap: 12px;
                background: #ffffff;
                border: 1px solid #e5e7eb;
                border-left-width: 4px;
                border-radius: 12px;
                padding: 14px 14px 16px 14px;
                box-shadow: 0 12px 28px -6px rgba(15, 23, 42, 0.16), 0 4px 10px -2px rgba(15, 23, 42, 0.08);
                font-family: 'Open Sans', system-ui, -apple-system, sans-serif;
                animation: uiaSlideIn 0.25s cubic-bezier(0.16, 1, 0.3, 1);
                transition: opacity 0.2s ease, transform 0.2s ease;
            }
            .uia-toast.uia-leaving {
                opacity: 0;
                transform: translateX(24px);
            }
            .uia-toast-icon {
                width: 32px;
                height: 32px;
                border-radius: 9999px;
                display: flex;
                align-items: center;
                justify-content: center;
                flex-shrink: 0;
                font-size: 15px;
            }
            .uia-toast-body {
                flex: 1;
                min-width: 0;
            }
            .uia-toast-title {
                font-size: 13px;
                font-weight: 700;
                color: #111827;
                line-height: 1.3;
                margin-bottom: 2px;
            }
            .uia-toast-msg {
                font-size: 13px;
                color: #4b5563;
                line-height: 1.45;
                white-space: pre-line;
                word-break: break-word;
            }
            .uia-toast-close {
                background: transparent;
                border: none;
                color: #9ca3af;
                cursor: pointer;
                padding: 2px 4px;
                border-radius: 6px;
                font-size: 13px;
                line-height: 1;
                flex-shrink: 0;
                transition: color 0.15s, background 0.15s;
            }
            .uia-toast-close:hover {
                color: #374151;
                background: #f3f4f6;
            }
            .uia-toast-progress {
                position: absolute;
                left: 0;
                bottom: 0;
                height: 3px;
                width: 100%;
                transform-origin: left;
                animation: uiaProgress linear forwards;
            }
            .uia-toast:hover .uia-toast-progress {
                animation-play-state: paused;
            }

            .uia-backdrop {
                position: fixed;
                inset: 0;
                z-index: 9999;
                background: rgba(15, 23, 42, 0.5);
                backdrop-filter: blur(4px);
                -webkit-backdrop-filter: blur(4px);
                display: flex;
                align-items: center;
                justify-content: center;
                padding: 16px;
                font-family: 'Open Sans', system-ui, -apple-system, sans-serif;
                animation: uiaFadeIn 0.18s ease-out;
            }
            .uia-modal {
                background: #ffffff;
                border-radius: 16px;
                width: 100%;
                max-width: 430px;
                box-shadow: 0 24px 48px -12px rgba(15, 23, 42, 0.28);
                border: 1px solid #e5e7eb;
                overflow: hidden;
                animation: uiaScaleIn 0.2s cubic-bezier(0.16, 1, 0.3, 1);
            }
            .uia-modal-content {
                padding: 26px 24px 20px 24px;
                text-align: center;
            }
            .uia-modal-icon {
                width: 56px;
                height: 56px;
                border-radius: 9999px;
                margin: 0 auto 14px auto;
                display: flex;
                align-items: center;
                justify-content: center;
                font-size: 24px;
            }
            .uia-modal-title {
                font-size: 18px;
                font-weight: 700;
                color: #111827;
                margin: 0 0 8px 0;
                line-height: 1.3;
            }
            .uia-modal-msg {
                font-size: 14px;
                color: #4b5563;
                margin: 0;
                line-height: 1.55;
                white-space: pre-line;
                word-break: break-word;
            }
            .uia-prompt-input {
                margin-top: 16px;
                width: 100%;
                box-sizing: border-box;
                padding: 10px 12px;
                border: 1px solid #d1d5db;
                border-radius: 10px;
                font-size: 14px;
                color: #111827;
                outline: none;
                transition: border-color 0.15s, box-shadow 0.15s;
            }
            .uia-prompt-input:focus {
                border-color: #2563eb;
                box-shadow: 0 0 0 3px rgba(37, 99, 235, 0.15);
            }
            .uia-modal-footer {
                padding: 14px 20px;
                background: #f9fafb;
                border-top: 1px solid #e5e7eb;
                display: flex;
                justify-content: flex-end;
                gap: 10px;
            }
            .uia-btn {
                border-radius: 9px;
                padding: 9px 18px;
                font-size: 13.5px;
                font-weight: 600;
                cursor: pointer;
                border: 1px solid transparent;
                transition: background 0.15s, border-color 0.15s, transform 0.1s;
                font-family: inherit;
            }
            .uia-btn:active {
                transform: scale(0.98);
            }
            .uia-btn-cancel {
                background: #ffffff;
                color: #374151;
                border-color: #d1d5db;
            }
            .uia-btn-cancel:hover {
                background: #f3f4f6;
            }
            .uia-btn-primary {
                background: #2563eb;
                color: #ffffff;
            }
            .uia-btn-primary:hover {
                background: #1d4ed8;
            }
            .uia-btn-danger {
                background: #dc2626;
                color: #ffffff;
            }
            .uia-btn-danger:hover {
                background: #b91c1c;
            }
            .uia-btn-success {
                background: #16a34a;
                color: #ffffff;
            }
            .uia-btn-success:hover {
                background: #15803d;
            }

            @keyframes uiaSlideIn {
                from { opacity: 0; transform: translateX(28px); }
                to   { opacity: 1; transform: translateX(0); }
            }
            @keyframes uiaFadeIn {
                from { opacity: 0; }
                to   { opacity: 1; }
            }
            @keyframes uiaScaleIn {
                from { opacity: 0; transform: scale(0.94) translateY(8px); }
                to   { opacity: 1; transform: scale(1) translateY(0); }
            }
            @keyframes uiaProgress {
                from { transform: scaleX(1); }
                to   { transform: scaleX(0); }
            }
        `;
        document.head.appendChild(style);
    }

    function getToastStack() {
        injectStyles();
        let stack = document.getElementById('uia-toast-stack');
        if (!stack) {
            stack = document.createElement('div');
            stack.id = 'uia-toast-stack';
            stack.className = 'uia-toast-stack';
            document.body.appendChild(stack);
        }
        return stack;
    }

    function toast(message, type = 'success', title = '', duration = 4200) {
        if (!document.body) {
            document.addEventListener('DOMContentLoaded', () => toast(message, type, title, duration), { once: true });
            return;
        }
        const t = THEME[type] || THEME.info;
        const stack = getToastStack();

        const el = document.createElement('div');
        el.className = 'uia-toast';
        el.style.borderLeftColor = t.accent;

        const iconWrap = document.createElement('div');
        iconWrap.className = 'uia-toast-icon';
        iconWrap.style.background = t.softBg;
        iconWrap.style.color = t.accent;
        const icon = document.createElement('i');
        icon.className = `fas ${t.icon}`;
        iconWrap.appendChild(icon);

        const body = document.createElement('div');
        body.className = 'uia-toast-body';
        const titleEl = document.createElement('div');
        titleEl.className = 'uia-toast-title';
        titleEl.textContent = title || t.defaultTitle;
        const msgEl = document.createElement('div');
        msgEl.className = 'uia-toast-msg';
        msgEl.textContent = String(message || '');
        body.appendChild(titleEl);
        body.appendChild(msgEl);

        const closeBtn = document.createElement('button');
        closeBtn.type = 'button';
        closeBtn.className = 'uia-toast-close';
        closeBtn.setAttribute('aria-label', 'Tutup');
        const closeIcon = document.createElement('i');
        closeIcon.className = 'fas fa-times';
        closeBtn.appendChild(closeIcon);

        const prog = document.createElement('div');
        prog.className = 'uia-toast-progress';
        prog.style.background = t.accent;
        prog.style.animationDuration = `${duration}ms`;

        el.appendChild(iconWrap);
        el.appendChild(body);
        el.appendChild(closeBtn);
        el.appendChild(prog);
        stack.appendChild(el);

        let closed = false;
        let remaining = duration;
        let startedAt = Date.now();
        let timerId = null;

        function dismiss() {
            if (closed) return;
            closed = true;
            if (timerId) clearTimeout(timerId);
            el.classList.add('uia-leaving');
            setTimeout(() => el.remove(), 220);
        }

        function startTimer() {
            startedAt = Date.now();
            timerId = setTimeout(dismiss, remaining);
        }

        el.addEventListener('mouseenter', () => {
            if (timerId) clearTimeout(timerId);
            remaining = Math.max(400, remaining - (Date.now() - startedAt));
        });
        el.addEventListener('mouseleave', () => {
            if (!closed) startTimer();
        });
        closeBtn.addEventListener('click', dismiss);

        startTimer();
    }

    function openDialog(opts) {
        injectStyles();
        return new Promise((resolve) => {
            const type = opts.type || 'info';
            const t = THEME[type] || THEME.info;

            const backdrop = document.createElement('div');
            backdrop.className = 'uia-backdrop';
            backdrop.setAttribute('role', 'dialog');
            backdrop.setAttribute('aria-modal', 'true');

            const modal = document.createElement('div');
            modal.className = 'uia-modal';

            const content = document.createElement('div');
            content.className = 'uia-modal-content';

            const iconWrap = document.createElement('div');
            iconWrap.className = 'uia-modal-icon';
            iconWrap.style.background = t.softBg;
            iconWrap.style.color = t.accent;
            const icon = document.createElement('i');
            icon.className = `fas ${opts.icon || t.icon}`;
            iconWrap.appendChild(icon);

            const titleEl = document.createElement('h3');
            titleEl.className = 'uia-modal-title';
            titleEl.textContent = opts.title || t.defaultTitle;

            const msgEl = document.createElement('p');
            msgEl.className = 'uia-modal-msg';
            msgEl.textContent = String(opts.message || '');

            content.appendChild(iconWrap);
            content.appendChild(titleEl);
            if (opts.message) content.appendChild(msgEl);

            let inputEl = null;
            if (opts.mode === 'prompt') {
                inputEl = document.createElement('input');
                inputEl.type = 'text';
                inputEl.className = 'uia-prompt-input';
                inputEl.value = opts.defaultValue != null ? String(opts.defaultValue) : '';
                inputEl.placeholder = opts.placeholder || '';
                content.appendChild(inputEl);
            }

            const footer = document.createElement('div');
            footer.className = 'uia-modal-footer';

            function cleanup(val) {
                document.removeEventListener('keydown', onKeyDown);
                backdrop.remove();
                resolve(val);
            }

            if (opts.mode === 'confirm' || opts.mode === 'prompt') {
                const cancelBtn = document.createElement('button');
                cancelBtn.type = 'button';
                cancelBtn.className = 'uia-btn uia-btn-cancel';
                cancelBtn.textContent = opts.cancelText || 'Batal';
                cancelBtn.addEventListener('click', () => cleanup(opts.mode === 'prompt' ? null : false));
                footer.appendChild(cancelBtn);
            }

            const confirmBtn = document.createElement('button');
            confirmBtn.type = 'button';
            let btnStyle = 'uia-btn-primary';
            if (opts.danger || type === 'error') btnStyle = 'uia-btn-danger';
            else if (type === 'success') btnStyle = 'uia-btn-success';
            confirmBtn.className = `uia-btn ${btnStyle}`;
            confirmBtn.textContent = opts.confirmText || (opts.mode === 'confirm' ? 'Ya, Lanjutkan' : (opts.mode === 'prompt' ? 'Simpan' : 'OK'));
            confirmBtn.addEventListener('click', () => {
                if (opts.mode === 'prompt') cleanup(inputEl.value);
                else cleanup(true);
            });
            footer.appendChild(confirmBtn);

            modal.appendChild(content);
            modal.appendChild(footer);
            backdrop.appendChild(modal);
            document.body.appendChild(backdrop);

            backdrop.addEventListener('click', (e) => {
                if (e.target === backdrop) {
                    if (opts.mode === 'alert') cleanup(true);
                    else if (opts.mode === 'prompt') cleanup(null);
                    else cleanup(false);
                }
            });

            function onKeyDown(e) {
                if (e.key === 'Escape') {
                    e.preventDefault();
                    if (opts.mode === 'alert') cleanup(true);
                    else if (opts.mode === 'prompt') cleanup(null);
                    else cleanup(false);
                } else if (e.key === 'Enter') {
                    e.preventDefault();
                    if (opts.mode === 'prompt') cleanup(inputEl.value);
                    else cleanup(true);
                }
            }
            document.addEventListener('keydown', onKeyDown);

            setTimeout(() => {
                if (inputEl) {
                    inputEl.focus();
                    inputEl.select();
                } else {
                    confirmBtn.focus();
                }
            }, 30);
        });
    }

    function alertModal(optionsOrMessage, type = 'info') {
        const opts = typeof optionsOrMessage === 'string'
            ? { message: optionsOrMessage, type }
            : { ...optionsOrMessage };
        return openDialog({ ...opts, mode: 'alert' });
    }

    function confirmModal(optionsOrMessage) {
        const opts = typeof optionsOrMessage === 'string'
            ? { message: optionsOrMessage, type: 'warning', title: 'Konfirmasi' }
            : { type: 'warning', title: 'Konfirmasi', ...optionsOrMessage };
        return openDialog({ ...opts, mode: 'confirm' });
    }

    function promptModal(optionsOrMessage, defaultValue = '') {
        const opts = typeof optionsOrMessage === 'string'
            ? { message: optionsOrMessage, defaultValue, type: 'info', title: 'Input Data' }
            : { type: 'info', title: 'Input Data', ...optionsOrMessage };
        return openDialog({ ...opts, mode: 'prompt' });
    }

    global.UIAlert = {
        toast,
        alert: alertModal,
        confirm: confirmModal,
        prompt: promptModal
    };
})(window);
