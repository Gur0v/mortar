(bind-key '(alt Return) (lambda () (spawn "x-terminal-emulator")))
(bind-key '(alt d) (lambda () (spawn "dmenu_run")))
(bind-key '(alt b) (lambda () (split 'horizontal)))
(bind-key '(alt v) (lambda () (split 'vertical)))
(bind-key '(alt e) toggle-split)
(bind-key '(alt a) focus-parent)

(for-each
 (lambda (entry)
   (let ((key (car entry)) (direction (cdr entry)))
     (bind-key (list 'alt key) (lambda () (focus direction)))
     (bind-key (list 'alt 'shift key) (lambda () (move direction)))))
 '((h . left) (j . down) (k . up) (l . right)
   (Left . left) (Down . down) (Up . up) (Right . right)))

(for-each
 (lambda (entry)
   (let ((key (car entry)) (direction (cdr entry)))
     (bind-key (list 'alt 'control key) (lambda () (resize direction 10)))))
 '((Left . left) (Down . down) (Up . up) (Right . right)))

(bind-key '(alt shift q) close)
(bind-key '(alt shift space) floating-toggle)
(bind-key '(alt space) focus-mode-toggle)
(bind-key '(alt f) fullscreen-toggle)
(bind-key '(alt shift minus) scratchpad-move)
(bind-key '(alt minus) scratchpad-show)
(bind-key '(alt shift c) reload)

(for-each
 (lambda (digit)
   (let ((key (string->symbol (number->string digit))))
     (bind-key (list 'alt key) (lambda () (workspace digit)))
     (bind-key (list 'alt 'shift key) (lambda () (move-to-workspace digit)))))
 '(1 2 3 4 5 6 7 8 9 0))
