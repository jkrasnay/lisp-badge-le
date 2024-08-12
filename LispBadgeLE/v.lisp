;; Visual editor for uLisp
;;
;; To evaluate, first start a screen session in a new terminal:
;;
;;   screen -S lisp /dev/cu.usbserial-ABSCEVQR
;;
;; The following command binds ,, to send the current line to the above screen session:
;;
;; :lua vim.keymap.set('n', ',,', function() vim.fn.system({"screen", "-S", "lisp", "-X", "stuff", vim.api.nvim_get_current_line()})
;;

;; Width of the screen, in columns
;(defvar v-cols 40)

;; Offset of the cursor
(defvar v-cursor 0)

;; Editor mode, 'n' or 'i'
;; 0 = Normal
;; 1 = Insert
(defvar v-mode 0)

;; Temporary buffer holding the string, until
;; we implement a proper buffer in our extensions
(defvar v-buf "hello")

;; Abstraction layer for the buffer
(defun v-bufstr () vbuf)
(defun v-buflen () (length v-buf))
(defun v-bufchar (i) (char v-buf i))

(defun v-offcol (offset) (mod offset 40))
(defun v-offline (offset) (/ offset 40))

(defun v-draw-status () (let ((s (format nil "~40a" (if (= 0 v-mode) "NORMAL" "INSERT")))) (dotimes (i (length s)) (plot-char (char s i) 0 i t)) ))

(defun v-draw-char (i invert) (plot-char (v-bufchar i) (1+ (v-offline i)) (v-offcol i) invert))
(defun v-draw-buf () (dotimes (i (v-buflen)) (v-draw-char i nil)))

(defun v-move (delta) (v-draw-char v-cursor nil) (setf v-cursor (max 0 (min (1- (v-buflen)) (+ v-cursor delta)))) (v-draw-char v-cursor t) nil)
(defun v-left () (v-move -1))
(defun v-right () (setf v-cursor (min (1+ v-cursor) (1- (v-buflen)))))

(defun v-normal (c) (case c (#\q :quit) (#\w (v-bufstr)) (#\h (v-move -1)) (#\l (v-move 1))))

(defun v-insert (c) )

(defun v-handle-char (c) (case v-mode (0 (v-normal c)) (t (v-insert c))))

(defun v-init (s) (setf v-buf s) (setf v-cursor 0) (fill-screen) (v-draw-status) (v-draw-buf) (v-move 0))
(defun v-loop () (with-kb-raw (loop (let* ((c (read-char)) (resp (v-handle-char c))) (when resp (fill-screen) (return resp))))))

(defun v (s) (v-init s) (v-loop))

(v "Hello, world!")

(with-kb-raw (read-char))

(save-image)
