;;;; Emacs Lisp help for writing Subversion code. ;;;;

;;; In C files, put something like this to load this file automatically:
;;
;;   /* -----------------------------------------------------------------
;;    * local variables:
;;    * eval: (load-file "../svn-dev.el")
;;    * end:
;;    */
;;
;; (note: make sure to get the path right in the argument to load-file).



(c-set-style "gnu")
(setq indent-tabs-mode nil)

(progn (message "loaded") (sit-for 1))



;; Much of the APR documentation is embedded perldoc format.  The
;; perldoc program itself sucks, however.  If you're the author of
;; perldoc, I'm sorry, but what were you thinking?  Don't you know
;; that there are people in the world who don't work in vt100
;; terminals?  If I want to view a perldoc page in my Emacs shell
;; buffer, I have to run the ridiculous command
;;
;;   $ PAGER=cat perldoc -t target_file
;;
;; (Not that this was documented anywhere, I had to figure it out for
;; myself by reading /usr/bin/perldoc).
;;
;; Non-paging behavior should be a standard command-line option.  No
;; program that can output text should *ever* insist on invoking the
;; pager.
;;
;; Anyway, these Emacs commands will solve the problem for us.
;;
;; Acknowledgements:
;; Much of this code is copied from man.el in the FSF Emacs 21.x
;; sources.

(defcustom svn-perldoc-overstrike-face 'bold
  "*Face to use when fontifying overstrike."
  :type 'face
  :group 'svn-dev)

(defcustom svn-perldoc-underline-face 'underline
  "*Face to use when fontifying underlining."
  :type 'face
  :group 'svn-dev)


(defun svn-perldoc-softhyphen-to-minus ()
  ;; \255 is some kind of dash in Latin-N.  Versions of Debian man, at
  ;; least, emit it even when not in a Latin-N locale.
  (unless (eq t (compare-strings "latin-" 0 nil
				 current-language-environment 0 6 t))
    (goto-char (point-min))
    (let ((str "\255"))
      (if enable-multibyte-characters
	  (setq str (string-as-multibyte str)))
      (while (search-forward str nil t) (replace-match "-")))))


(defun svn-perldoc-fontify-buffer ()
  "Convert overstriking and underlining to the correct fonts.
Same for the ANSI bold and normal escape sequences."
  (interactive)
  (message "Please wait, making up the page...")
  (goto-char (point-min))
  (while (search-forward "\e[1m" nil t)
    (delete-backward-char 4)
    (put-text-property (point)
		       (progn (if (search-forward "\e[0m" nil 'move)
				  (delete-backward-char 4))
			      (point))
		       'face svn-perldoc-overstrike-face))
  (goto-char (point-min))
  (while (search-forward "_\b" nil t)
    (backward-delete-char 2)
    (put-text-property (point) (1+ (point)) 'face svn-perldoc-underline-face))
  (goto-char (point-min))
  (while (search-forward "\b_" nil t)
    (backward-delete-char 2)
    (put-text-property (1- (point)) (point) 'face svn-perldoc-underline-face))
  (goto-char (point-min))
  (while (re-search-forward "\\(.\\)\\(\b\\1\\)+" nil t)
    (replace-match "\\1")
    (put-text-property (1- (point)) (point) 'face svn-perldoc-overstrike-face))
  (goto-char (point-min))
  (while (re-search-forward "o\b\\+\\|\\+\bo" nil t)
    (replace-match "o")
    (put-text-property (1- (point)) (point) 'face 'bold))
  (goto-char (point-min))
  (while (re-search-forward "[-|]\\(\b[-|]\\)+" nil t)
    (replace-match "+")
    (put-text-property (1- (point)) (point) 'face 'bold))
  (svn-perldoc-softhyphen-to-minus)
  (message "Please wait, making up the page...done"))


(defun svn-perldoc-cleanup-buffer ()
  "Remove overstriking and underlining from the current buffer."
  (interactive)
  (message "Please wait, cleaning up the page...")
  (progn
    (goto-char (point-min))
    (while (search-forward "_\b" nil t) (backward-delete-char 2))
    (goto-char (point-min))
    (while (search-forward "\b_" nil t) (backward-delete-char 2))
    (goto-char (point-min))
    (while (re-search-forward "\\(.\\)\\(\b\\1\\)+" nil t) 
      (replace-match "\\1"))
    (goto-char (point-min))
    (while (re-search-forward "\e\\[[0-9]+m" nil t) (replace-match ""))
    (goto-char (point-min))
    (while (re-search-forward "o\b\\+\\|\\+\bo" nil t) (replace-match "o"))
    (goto-char (point-min))
    (while (re-search-forward "" nil t) (replace-match " ")))
  (goto-char (point-min))
  (while (re-search-forward "[-|]\\(\b[-|]\\)+" nil t) (replace-match "+"))
  (svn-perldoc-softhyphen-to-minus)
  (message "Please wait, cleaning up the page...done"))


;; Entry point to svn-perldoc functionality.
(defun svn-perldoc (file)
  "Run perldoc on FILE, display the output in a buffer."
  (interactive "fRun perldoc on file: ")
  (let ((outbuf (get-buffer-create 
                 (format "*%s PerlDoc*" (file-name-nondirectory file))))
        (savepg (getenv "PAGER")))
    (setenv "PAGER" "cat")  ;; for perldoc
    (save-excursion
      (set-buffer outbuf)
      (delete-region (point-min) (point-max))
      (call-process "perldoc" nil outbuf nil (expand-file-name file))
      (svn-perldoc-fontify-buffer)      
      (svn-perldoc-cleanup-buffer)
      ;; Clean out the inevitable leading dead space.
      (goto-char (point-min))
      (re-search-forward "[^ \i\n]")
      (beginning-of-line)
      (delete-region (point-min) (point)))
    (setenv "PAGER" savepg)
    (display-buffer outbuf)))

