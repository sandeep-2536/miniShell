#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <ctype.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <readline/readline.h>
#include <readline/history.h>

/*
** environ: declared by the C library, points to the process's current
** environment (the array execve's 3rd argument is normally built from).
** We use it as-is for now; Step 12 replaces this with our OWN managed
** copy so that "export"/"unset" can actually modify it.
*/
extern char	**environ;

/* ============================ TOKEN TYPES ============================ */

typedef enum e_token_type
{
	TOKEN_WORD,
	TOKEN_PIPE,       // |
	TOKEN_REDIR_IN,   // <
	TOKEN_REDIR_OUT,  // >
	TOKEN_APPEND,     // >>
	TOKEN_HEREDOC     // <<
}	t_token_type;

typedef struct s_token
{
	t_token_type		type;
	char				*value;
	char				*expand_mask;	/* per-char: '1' = protected (no $ expansion), '0' = expandable. NULL for non-WORD tokens. */
	struct s_token		*next;
}	t_token;

/* ============================ COMMAND TYPES ============================ */

/*
** t_redir: one redirection attached to a command.
** e.g. "cmd > out.txt"  -> type = TOKEN_REDIR_OUT, file = "out.txt"
** A command can have several (linked list), the LAST of a given
** direction wins at execution time (that's how bash behaves too).
*/
typedef struct s_redir
{
	t_token_type		type;
	char				*file;
	char				*heredoc_content;	/* only used for TOKEN_HEREDOC: the collected body text */
	int					heredoc_expand;		/* 1 = expand $VAR in body lines, 0 = literal (quoted delimiter) */
	struct s_redir		*next;
}	t_redir;

/*
** t_cmd: one command in a pipeline.
** argv   -> NULL-terminated array ready to hand to execve() later
** redirs -> linked list of this command's redirections
** next   -> the next command in the pipeline (NULL if this is the last)
*/
typedef struct s_cmd
{
	char				**argv;
	int					argc;
	t_redir				*redirs;
	struct s_cmd		*next;
}	t_cmd;

/* ============================ LEXER ============================ */

/*
** new_token: allocates a single token node with given type/value.
*/
static t_token	*new_token(t_token_type type, char *value)
{
	t_token	*tok;

	tok = malloc(sizeof(t_token));
	if (!tok)
		return (NULL);
	tok->type = type;
	tok->value = value;
	tok->expand_mask = NULL;
	tok->next = NULL;
	return (tok);
}

/*
** add_token: appends a token node to the end of the token list.
*/
static void	add_token(t_token **head, t_token **tail, t_token *new)
{
	if (!*head)
		*head = new;
	else
		(*tail)->next = new;
	*tail = new;
}

/*
** is_operator_char: true if c starts one of our operators (| < >).
*/
static int	is_operator_char(char c)
{
	return (c == '|' || c == '<' || c == '>');
}

/*
** read_word: consumes one WORD token starting at line[*i], AND builds a
** same-length "expand mask" alongside it:
**   mask[k] = '1' -> that character came from inside single quotes,
**             so expansion (Step 6) must leave it untouched.
**   mask[k] = '0' -> bare character or from inside double quotes,
**             so expansion IS allowed to act on it.
** Quoted and unquoted segments still glue into one token as before.
** Sets *err = 1 and returns NULL if a quote is never closed.
** *mask_out receives the mask string (caller must free it).
*/
static char	*read_word(const char *line, int *i, int *err, char **mask_out)
{
	char	*buf;
	char	*mask;
	int		start;
	int		len;
	char	quote;

	buf = malloc(strlen(line) + 1);
	mask = malloc(strlen(line) + 1);
	if (!buf || !mask)
		return (NULL);
	len = 0;
	while (line[*i] && line[*i] != ' ' && !is_operator_char(line[*i]))
	{
		if (line[*i] == '\'' || line[*i] == '"')
		{
			quote = line[*i];
			(*i)++;
			start = *i;
			while (line[*i] && line[*i] != quote)
				(*i)++;
			if (!line[*i])
			{
				*err = 1;
				free(buf);
				free(mask);
				return (NULL);
			}
			while (start < *i)
			{
				buf[len] = line[start];
				mask[len] = (quote == '\'') ? '1' : '0';
				len++;
				start++;
			}
			(*i)++;
		}
		else
		{
			mask[len] = '0';
			buf[len++] = line[(*i)++];
		}
	}
	buf[len] = '\0';
	mask[len] = '\0';
	*mask_out = mask;
	return (buf);
}

/*
** tokenize: turns the raw input line into a linked list of tokens.
** Handles words (with quote merging) and the operators | < > >> <<.
** On unterminated-quote error: frees everything built so far,
** sets *err = 1, and returns NULL.
*/
t_token	*tokenize(const char *line, int *err)
{
	t_token	*head;
	t_token	*tail;
	int		i;
	char	*word;
	char	*mask;
	t_token	*tok;

	head = NULL;
	tail = NULL;
	*err = 0;
	i = 0;
	while (line[i])
	{
		if (line[i] == ' ')
		{
			i++;
			continue ;
		}
		if (line[i] == '|')
		{
			add_token(&head, &tail, new_token(TOKEN_PIPE, strdup("|")));
			i++;
		}
		else if (line[i] == '<')
		{
			if (line[i + 1] == '<')
			{
				add_token(&head, &tail, new_token(TOKEN_HEREDOC, strdup("<<")));
				i += 2;
			}
			else
			{
				add_token(&head, &tail, new_token(TOKEN_REDIR_IN, strdup("<")));
				i++;
			}
		}
		else if (line[i] == '>')
		{
			if (line[i + 1] == '>')
			{
				add_token(&head, &tail, new_token(TOKEN_APPEND, strdup(">>")));
				i += 2;
			}
			else
			{
				add_token(&head, &tail, new_token(TOKEN_REDIR_OUT, strdup(">")));
				i++;
			}
		}
		else
		{
			word = read_word(line, &i, err, &mask);
			if (*err)
			{
				while (head)
				{
					tail = head->next;
					free(head->value);
					free(head->expand_mask);
					free(head);
					head = tail;
				}
				return (NULL);
			}
			tok = new_token(TOKEN_WORD, word);
			tok->expand_mask = mask;
			add_token(&head, &tail, tok);
		}
	}
	return (head);
}

/*
** free_tokens: releases the whole token list (values + nodes).
*/
void	free_tokens(t_token *head)
{
	t_token	*tmp;

	while (head)
	{
		tmp = head->next;
		free(head->value);
		free(head->expand_mask);
		free(head);
		head = tmp;
	}
}

/* ============================ EXPANSION ============================ */

/*
** t_dstr: a tiny growable string buffer. Expansion can shrink or grow
** a word's length (e.g. "$?" -> "1", or an unset "$FOO" -> ""), so we
** can't just reuse the original buffer size — we build the result
** incrementally, doubling capacity whenever we run out of room.
*/
typedef struct s_dstr
{
	char	*data;
	int		len;
	int		cap;
}	t_dstr;

/*
** dstr_init: sets up an empty growable string with a small starting capacity.
*/
static void	dstr_init(t_dstr *d)
{
	d->cap = 32;
	d->data = malloc(d->cap);
	d->data[0] = '\0';
	d->len = 0;
}

/*
** dstr_append_char: appends one character, growing the buffer if needed.
*/
static void	dstr_append_char(t_dstr *d, char c)
{
	char	*bigger;

	if (d->len + 2 > d->cap)
	{
		d->cap *= 2;
		bigger = realloc(d->data, d->cap);
		d->data = bigger;
	}
	d->data[d->len++] = c;
	d->data[d->len] = '\0';
}

/*
** dstr_append_str: appends a whole C string, one char at a time.
*/
static void	dstr_append_str(t_dstr *d, const char *s)
{
	while (s && *s)
		dstr_append_char(d, *s++);
}

/*
** expand_word: walks "word" alongside its "mask" (from the lexer) and
** builds a new string with substitutions applied:
**   $?         -> the given exit_status, as a string
**   $NAME      -> getenv("NAME"), or "" if unset
**   bare '$'   -> left as a literal '$' (not followed by a valid name/? )
** Anywhere mask[i] == '1' (single-quoted at lex time), characters are
** copied through completely untouched, '$' included.
** Returns a freshly malloc'd string; caller owns it.
*/
static char	*expand_word(const char *word, const char *mask, int exit_status)
{
	t_dstr	d;
	int		i;
	int		j;
	char	*name;
	char	*value;
	char	status_str[16];

	dstr_init(&d);
	i = 0;
	while (word[i])
	{
		if (mask[i] == '0' && word[i] == '$' && word[i + 1] == '?')
		{
			sprintf(status_str, "%d", exit_status);
			dstr_append_str(&d, status_str);
			i += 2;
		}
		else if (mask[i] == '0' && word[i] == '$'
			&& (isalpha((unsigned char)word[i + 1]) || word[i + 1] == '_'))
		{
			j = i + 1;
			while (word[j] && (isalnum((unsigned char)word[j]) || word[j] == '_'))
				j++;
			name = strndup(word + i + 1, j - (i + 1));
			value = getenv(name);
			dstr_append_str(&d, value);
			free(name);
			i = j;
		}
		else
			dstr_append_char(&d, word[i++]);
	}
	return (d.data);
}

/*
** all_protected: true if every character in "mask" is '1', i.e. the
** whole word came from inside single quotes. Used for heredoc
** delimiters: "<<EOF" and "<<\"EOF\"" allow $VAR expansion in the
** heredoc body, but "<<'EOF'" (fully quoted) does not — matching bash.
*/
static int	all_protected(const char *mask)
{
	int	i;

	if (!mask || !mask[0])
		return (0);
	i = 0;
	while (mask[i])
	{
		if (mask[i] != '1')
			return (0);
		i++;
	}
	return (1);
}

/* ============================ PARSER ============================ */

/*
** new_cmd: allocates an empty command node (no args, no redirs yet).
*/
static t_cmd	*new_cmd(void)
{
	t_cmd	*cmd;

	cmd = malloc(sizeof(t_cmd));
	if (!cmd)
		return (NULL);
	cmd->argv = NULL;
	cmd->argc = 0;
	cmd->redirs = NULL;
	cmd->next = NULL;
	return (cmd);
}

/*
** cmd_add_arg: appends one word to cmd->argv, keeping it NULL-terminated.
** Reallocates the array each call — fine at shell-input scale.
*/
static void	cmd_add_arg(t_cmd *cmd, char *word)
{
	char	**new_argv;
	int		i;

	new_argv = malloc(sizeof(char *) * (cmd->argc + 2));
	i = 0;
	while (i < cmd->argc)
	{
		new_argv[i] = cmd->argv[i];
		i++;
	}
	new_argv[cmd->argc] = word;
	new_argv[cmd->argc + 1] = NULL;
	free(cmd->argv);
	cmd->argv = new_argv;
	cmd->argc++;
}

/*
** cmd_add_redir: appends a new redirection to the end of cmd->redirs.
** "file" must already be a malloc'd string the caller is handing over
** ownership of (either expanded, or strdup'd for heredoc delimiters).
** Returns the new node so the caller can set heredoc-specific fields.
*/
static t_redir	*cmd_add_redir(t_cmd *cmd, t_token_type type, char *file)
{
	t_redir	*r;
	t_redir	*tmp;

	r = malloc(sizeof(t_redir));
	r->type = type;
	r->file = file;
	r->heredoc_content = NULL;
	r->heredoc_expand = 0;
	r->next = NULL;
	if (!cmd->redirs)
		cmd->redirs = r;
	else
	{
		tmp = cmd->redirs;
		while (tmp->next)
			tmp = tmp->next;
		tmp->next = r;
	}
	return (r);
}

/*
** add_cmd: appends a finished command to the pipeline list.
*/
static void	add_cmd(t_cmd **head, t_cmd **tail, t_cmd *new)
{
	if (!*head)
		*head = new;
	else
		(*tail)->next = new;
	*tail = new;
}

/*
** is_redir: true if this token type is one of the 4 redirection operators.
*/
static int	is_redir(t_token_type type)
{
	return (type == TOKEN_REDIR_IN || type == TOKEN_REDIR_OUT
		|| type == TOKEN_APPEND || type == TOKEN_HEREDOC);
}

/*
** free_cmd: releases one command node (its argv strings + redir list).
*/
static void	free_cmd(t_cmd *cmd)
{
	t_redir	*r;
	int		i;

	if (!cmd)
		return ;
	i = 0;
	while (cmd->argv && cmd->argv[i])
		free(cmd->argv[i++]);
	free(cmd->argv);
	while (cmd->redirs)
	{
		r = cmd->redirs->next;
		free(cmd->redirs->file);
		free(cmd->redirs->heredoc_content);
		free(cmd->redirs);
		cmd->redirs = r;
	}
	free(cmd);
}

/*
** free_cmds: releases the whole pipeline (every command node).
*/
void	free_cmds(t_cmd *head)
{
	t_cmd	*tmp;

	while (head)
	{
		tmp = head->next;
		free_cmd(head);
		head = tmp;
	}
}

/*
** parse_tokens: turns the flat token list into a pipeline of t_cmd,
** applying $VAR / $? expansion (Step 6) as each WORD becomes an argv
** entry or a redirection target.
** - TOKEN_WORD           -> expanded, then pushed into current argv
** - TOKEN_PIPE           -> current command is finished, start a new one
** - redirection operator -> must be followed by a WORD (the target).
**   That target is expanded too, EXCEPT for a heredoc delimiter, which
**   bash keeps literal (expansion inside the heredoc body comes later).
** Syntax errors caught here: empty command around a pipe
** ("| cmd", "cmd |", "cmd1 || cmd2") and a redirection with no filename.
** On success returns the pipeline head; on error returns NULL, *err = 1.
*/
t_cmd	*parse_tokens(t_token *tokens, int *err, int exit_status)
{
	t_cmd	*head;
	t_cmd	*tail;
	t_cmd	*cur;
	t_token	*t;
	char	*file;

	*err = 0;
	head = NULL;
	tail = NULL;
	if (!tokens)
		return (NULL);
	cur = new_cmd();
	t = tokens;
	while (t)
	{
		if (t->type == TOKEN_WORD)
			cmd_add_arg(cur, expand_word(t->value, t->expand_mask, exit_status));
		else if (t->type == TOKEN_PIPE)
		{
			if (cur->argc == 0 && !cur->redirs)
			{
				*err = 1;
				break ;
			}
			add_cmd(&head, &tail, cur);
			cur = new_cmd();
		}
		else if (is_redir(t->type))
		{
			t_redir	*r;

			if (!t->next || t->next->type != TOKEN_WORD)
			{
				*err = 1;
				break ;
			}
			if (t->type == TOKEN_HEREDOC)
			{
				file = strdup(t->next->value);
				r = cmd_add_redir(cur, t->type, file);
				r->heredoc_expand = !all_protected(t->next->expand_mask);
			}
			else
			{
				file = expand_word(t->next->value, t->next->expand_mask,
						exit_status);
				cmd_add_redir(cur, t->type, file);
			}
			t = t->next;
		}
		t = t->next;
	}
	if (*err)
	{
		free_cmd(cur);
		free_cmds(head);
		return (NULL);
	}
	if (cur->argc == 0 && !cur->redirs)
	{
		free_cmd(cur);
		*err = 1;
		free_cmds(head);
		return (NULL);
	}
	add_cmd(&head, &tail, cur);
	return (head);
}

/* ============================ DEBUG PRINTING ============================ */

/*
** token_type_name / redir_type_name: helpers for readable debug output.
*/
static const char	*redir_type_name(t_token_type type)
{
	if (type == TOKEN_REDIR_IN)
		return ("<");
	if (type == TOKEN_REDIR_OUT)
		return (">");
	if (type == TOKEN_APPEND)
		return (">>");
	return ("<<(heredoc)");
}

/*
** print_cmds: prints the parsed pipeline so we can visually verify
** the parser did the right thing. Purely a debugging aid for this
** stage — will be removed once execution exists.
*/
void	print_cmds(t_cmd *head)
{
	t_redir	*r;
	int		i;
	int		n;

	n = 1;
	while (head)
	{
		printf("cmd %d: argv = [", n);
		i = 0;
		while (head->argv && head->argv[i])
		{
			printf("\"%s\"%s", head->argv[i], head->argv[i + 1] ? ", " : "");
			i++;
		}
		printf("]\n");
		r = head->redirs;
		while (r)
		{
			printf("        redir %s \"%s\"\n", redir_type_name(r->type), r->file);
			r = r->next;
		}
		if (head->next)
			printf("   -- piped into --\n");
		head = head->next;
		n++;
	}
}

/* ============================ SIGNALS ============================ */

/*
** Forward declarations: run_child (below) needs to check whether a
** command is a builtin and dispatch to it, but the builtin
** implementations live further down in the file (BUILTINS section) for
** readability. These two lines just tell the compiler they exist.
*/
static int	is_builtin(char *name);
static int	execute_builtin(t_cmd *cmd, int *should_exit, int last_status);

/*
** sigint_handler: runs when Ctrl-C arrives WHILE WE'RE SITTING AT THE
** PROMPT (readline is blocked waiting for input). Real bash doesn't
** exit on Ctrl-C — it just abandons the current input line and shows
** a fresh prompt. We replicate that: print a newline, tell readline
** to discard whatever was typed so far, and redraw.
*/
static void	sigint_handler(int sig)
{
	(void)sig;
	write(STDOUT_FILENO, "\n", 1);
	rl_on_new_line();
	rl_replace_line("", 0);
	rl_redisplay();
}

/*
** set_signals_interactive: signal setup while WAITING AT THE PROMPT.
**   SIGINT  -> our handler above (abandon line, redraw prompt)
**   SIGQUIT -> ignored (bash's interactive shell never quits on Ctrl-\)
** SA_RESTART matters: without it, the interrupted readline() call could
** return an error instead of just being retried after our handler runs.
*/
static void	set_signals_interactive(void)
{
	struct sigaction	sa;

	sa.sa_handler = sigint_handler;
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = SA_RESTART;
	sigaction(SIGINT, &sa, NULL);
	signal(SIGQUIT, SIG_IGN);
}

/*
** set_signals_executing: signal setup while WAITING FOR A CHILD.
** The shell itself ignores both signals here — it's the CHILD that
** should react (each child resets to default right after fork, below).
** This is what lets Ctrl-C kill a running "sleep 100" without killing
** the shell that's waiting on it.
*/
static void	set_signals_executing(void)
{
	signal(SIGINT, SIG_IGN);
	signal(SIGQUIT, SIG_IGN);
}

/* ============================ HEREDOC ============================ */

/*
** read_heredoc_body: interactively reads lines (prompt "> ") until one
** is exactly equal to "delimiter", concatenating them (each followed by
** a newline) into one malloc'd string — this becomes that heredoc's
** stdin content. If expand_flag is set, $VAR/$? are applied per line
** (an unquoted "<<EOF" expands; a quoted "<<'EOF'" does not).
** Ctrl-D before the delimiter shows up ends the heredoc early, same as bash.
*/
static char	*read_heredoc_body(const char *delimiter, int expand_flag,
		int exit_status)
{
	t_dstr	d;
	char	*line;
	char	*expanded;
	char	*full_mask;

	dstr_init(&d);
	while (1)
	{
		line = readline("> ");
		if (!line)
			break ;
		if (!strcmp(line, delimiter))
		{
			free(line);
			break ;
		}
		if (expand_flag)
		{
			full_mask = malloc(strlen(line) + 1);
			memset(full_mask, '0', strlen(line));
			full_mask[strlen(line)] = '\0';
			expanded = expand_word(line, full_mask, exit_status);
			free(full_mask);
			dstr_append_str(&d, expanded);
			free(expanded);
		}
		else
			dstr_append_str(&d, line);
		dstr_append_char(&d, '\n');
		free(line);
	}
	return (d.data);
}

/*
** collect_heredocs: walks EVERY command in the pipeline and reads the
** body of each heredoc redirection up front, before any forking happens.
** This has to happen here (in the main shell, reading real stdin) rather
** than inside a child — by the time a child exists, its stdin may
** already be wired to a pipe instead of the terminal.
*/
static void	collect_heredocs(t_cmd *cmds, int exit_status)
{
	t_redir	*r;

	while (cmds)
	{
		r = cmds->redirs;
		while (r)
		{
			if (r->type == TOKEN_HEREDOC)
				r->heredoc_content = read_heredoc_body(r->file,
						r->heredoc_expand, exit_status);
			r = r->next;
		}
		cmds = cmds->next;
	}
}

/* ============================ EXECUTION ============================ */

/*
** find_executable: resolves a command name to a runnable file path.
**   - if "name" already contains a '/', treat it as given (relative or
**     absolute) and just check it's executable — no PATH search.
**   - otherwise, walk each ':'-separated directory in $PATH, and test
**     "<dir>/<name>" with access(X_OK) until one works.
** Returns a malloc'd path on success, NULL if nothing executable was found.
** (Caller is responsible for freeing the returned string.)
*/
static char	*find_executable(char *name)
{
	char	*path_env;
	char	*full;
	int		i;
	int		start;
	int		dirlen;

	if (strchr(name, '/'))
	{
		if (access(name, X_OK) == 0)
			return (strdup(name));
		return (NULL);
	}
	path_env = getenv("PATH");
	if (!path_env)
		return (NULL);
	i = 0;
	while (path_env[i] != '\0')
	{
		start = i;
		while (path_env[i] != ':' && path_env[i] != '\0')
			i++;
		dirlen = i - start;
		full = malloc(dirlen + 1 + strlen(name) + 1);
		memcpy(full, path_env + start, dirlen);
		full[dirlen] = '/';
		strcpy(full + dirlen + 1, name);
		if (access(full, X_OK) == 0)
			return (full);
		free(full);
		if (path_env[i] == ':')
			i++;
	}
	return (NULL);
}

/*
** apply_redirs: applies every redirection on this command, IN ORDER, by
** opening the right fd and dup2()-ing it onto stdin(0) or stdout(1).
** Because later redirections simply overwrite the fd via dup2, the LAST
** redirection of a given direction naturally "wins" for "cmd < a < b".
** Heredocs use a temp file (tmpfile()) rather than a live pipe — with a
** pipe, a large heredoc body could deadlock (writer blocks on a full
** pipe with nobody reading yet); a temp file has no such size limit.
** Returns 0 on success, -1 on failure (caller should treat as a failed command).
*/
static int	apply_redirs(t_cmd *cmd)
{
	t_redir	*r;
	int		fd;
	FILE	*tmp;

	r = cmd->redirs;
	while (r)
	{
		if (r->type == TOKEN_REDIR_IN)
		{
			fd = open(r->file, O_RDONLY);
			if (fd < 0)
				return (perror(r->file), -1);
			dup2(fd, STDIN_FILENO);
			close(fd);
		}
		else if (r->type == TOKEN_REDIR_OUT || r->type == TOKEN_APPEND)
		{
			if (r->type == TOKEN_APPEND)
				fd = open(r->file, O_WRONLY | O_CREAT | O_APPEND, 0644);
			else
				fd = open(r->file, O_WRONLY | O_CREAT | O_TRUNC, 0644);
			if (fd < 0)
				return (perror(r->file), -1);
			dup2(fd, STDOUT_FILENO);
			close(fd);
		}
		else
		{
			tmp = tmpfile();
			if (!tmp)
				return (perror("minishell: heredoc"), -1);
			fwrite(r->heredoc_content, 1, strlen(r->heredoc_content), tmp);
			rewind(tmp);
			dup2(fileno(tmp), STDIN_FILENO);
			fclose(tmp);
		}
		r = r->next;
	}
	return (0);
}

/*
** apply_redirs_only: handles a line that is PURE redirection with no
** command at all, e.g. "> file.txt" or "< in.txt" (valid in real bash —
** it just creates/truncates/reads the file and does nothing else).
** We must not call is_builtin()/execute_builtin() here because argv is
** NULL in this case, and argv[0] would be a NULL-pointer dereference —
** this exact case used to crash the shell before this fix.
*/
static int	apply_redirs_only(t_cmd *cmd)
{
	int	saved_in;
	int	saved_out;
	int	result;

	saved_in = dup(STDIN_FILENO);
	saved_out = dup(STDOUT_FILENO);
	if (apply_redirs(cmd) < 0)
		result = 1;
	else
		result = 0;
	fflush(stdout);
	dup2(saved_in, STDIN_FILENO);
	dup2(saved_out, STDOUT_FILENO);
	close(saved_in);
	close(saved_out);
	return (result);
}

/*
** run_builtin_with_redirs: a builtin that is the ONLY command on the line
** but DOES have redirections (e.g. "pwd > out.txt"). We must NOT fork
** here — forking would make "cd /tmp > log.txt" change the directory of
** a throwaway child instead of the real shell. Instead we temporarily
** redirect the shell's own stdin/stdout, run the builtin in-process,
** then restore the originals.
*/
static int	run_builtin_with_redirs(t_cmd *cmd, int *should_exit,
		int last_status)
{
	int	saved_in;
	int	saved_out;
	int	result;

	saved_in = dup(STDIN_FILENO);
	saved_out = dup(STDOUT_FILENO);
	if (apply_redirs(cmd) < 0)
		result = 1;
	else
		result = execute_builtin(cmd, should_exit, last_status);
	fflush(stdout);
	dup2(saved_in, STDIN_FILENO);
	dup2(saved_out, STDOUT_FILENO);
	close(saved_in);
	close(saved_out);
	return (result);
}

/*
** run_child: the code path executed INSIDE a forked child, whether it's
** a builtin or an external command. Never returns — always exit()s.
** Signals reset to default so Ctrl-C/Ctrl-\ behave normally for THIS
** process specifically (the parent shell keeps ignoring them, see
** set_signals_executing above).
*/
static void	run_child(t_cmd *cmd)
{
	char	*path;
	int		dummy_should_exit;

	signal(SIGINT, SIG_DFL);
	signal(SIGQUIT, SIG_DFL);
	if (apply_redirs(cmd) < 0)
		exit(1);
	if (!cmd->argv || !cmd->argv[0])
		exit(0);
	if (is_builtin(cmd->argv[0]))
	{
		dummy_should_exit = 0;
		exit(execute_builtin(cmd, &dummy_should_exit, 0));
	}
	path = find_executable(cmd->argv[0]);
	if (!path)
	{
		fprintf(stderr, "minishell: %s: command not found\n", cmd->argv[0]);
		exit(127);
	}
	execve(path, cmd->argv, environ);
	perror("minishell: execve");
	exit(126);
}

/*
** status_from_wait: converts a raw waitpid() status into bash's exit
** code convention: normal exit -> that code; killed by signal -> 128+sig.
** Also prints the classic "Quit (core dumped)" notice for SIGQUIT,
** matching what a real shell shows when a foreground command is quit.
*/
static int	status_from_wait(int status)
{
	if (WIFEXITED(status))
		return (WEXITSTATUS(status));
	if (WIFSIGNALED(status))
	{
		if (WTERMSIG(status) == SIGQUIT)
			printf("Quit (core dumped)\n");
		return (128 + WTERMSIG(status));
	}
	return (1);
}

/*
** execute_pipeline: runs a full pipeline of 1..N commands, wiring each
** command's stdout to the next command's stdin via pipe(). Every command
** (builtin or external) is forked here — this function is only reached
** when there's more than one command, OR a single external command, OR
** a single builtin WITH redirections is instead handled separately
** (see run_builtin_with_redirs) to keep builtins able to affect the shell.
** Returns the exit status of the LAST command — the standard $? convention
** for a pipeline.
*/
static int	execute_pipeline(t_cmd *cmds)
{
	int		n;
	t_cmd	*c;
	int		(*pipes)[2];
	pid_t	*pids;
	int		i;
	int		status;
	int		last_status;

	n = 0;
	c = cmds;
	while (c)
	{
		n++;
		c = c->next;
	}
	pipes = malloc(sizeof(*pipes) * (n > 1 ? n - 1 : 1));
	pids = malloc(sizeof(pid_t) * n);
	i = 0;
	while (i < n - 1)
	{
		if (pipe(pipes[i]) < 0)
		{
			perror("minishell: pipe");
			free(pipes);
			free(pids);
			return (1);
		}
		i++;
	}
	set_signals_executing();
	c = cmds;
	i = 0;
	while (c)
	{
		pids[i] = fork();
		if (pids[i] < 0)
		{
			perror("minishell: fork");
			c = c->next;
			i++;
			continue ;
		}
		if (pids[i] == 0)
		{
			if (i > 0)
				dup2(pipes[i - 1][0], STDIN_FILENO);
			if (i < n - 1)
				dup2(pipes[i][1], STDOUT_FILENO);
			status = 0;
			while (status < n - 1)
			{
				close(pipes[status][0]);
				close(pipes[status][1]);
				status++;
			}
			run_child(c);
		}
		c = c->next;
		i++;
	}
	i = 0;
	while (i < n - 1)
	{
		close(pipes[i][0]);
		close(pipes[i][1]);
		i++;
	}
	i = 0;
	last_status = 0;
	while (i < n)
	{
		if (pids[i] < 0)
		{
			if (i == n - 1)
				last_status = 1;
			i++;
			continue ;
		}
		waitpid(pids[i], &status, 0);
		if (i == n - 1)
			last_status = status_from_wait(status);
		i++;
	}
	set_signals_interactive();
	free(pipes);
	free(pids);
	return (last_status);
}

/* ============================ BUILTINS ============================ */

/*
** is_builtin: true if this command name is one we must run in-process
** (not forked) — see the fork/exec explanation above for why.
*/
static int	is_builtin(char *name)
{
	return (!strcmp(name, "cd") || !strcmp(name, "pwd")
		|| !strcmp(name, "echo") || !strcmp(name, "export")
		|| !strcmp(name, "unset") || !strcmp(name, "env")
		|| !strcmp(name, "exit"));
}

/*
** builtin_cd: changes the shell's OWN working directory via chdir().
** No argument -> go to $HOME (bash's default). Fails loudly on bad path.
*/
static int	builtin_cd(char **argv, int argc)
{
	char	*target;

	if (argc < 2)
	{
		target = getenv("HOME");
		if (!target)
		{
			fprintf(stderr, "minishell: cd: HOME not set\n");
			return (1);
		}
	}
	else
		target = argv[1];
	if (chdir(target) != 0)
	{
		perror("minishell: cd");
		return (1);
	}
	return (0);
}

/*
** builtin_pwd: prints the current working directory via getcwd().
*/
static int	builtin_pwd(void)
{
	char	cwd[4096];

	if (getcwd(cwd, sizeof(cwd)))
	{
		printf("%s\n", cwd);
		return (0);
	}
	perror("minishell: pwd");
	return (1);
}

/*
** builtin_echo: prints its arguments space-separated, with a trailing
** newline — unless the first argument is exactly "-n", which suppresses it
** (bash's classic echo -n behavior).
*/
static int	builtin_echo(char **argv, int argc)
{
	int	i;
	int	newline;

	newline = 1;
	i = 1;
	if (i < argc && !strcmp(argv[i], "-n"))
	{
		newline = 0;
		i++;
	}
	while (i < argc)
	{
		printf("%s", argv[i]);
		if (i + 1 < argc)
			printf(" ");
		i++;
	}
	if (newline)
		printf("\n");
	return (0);
}

/*
** builtin_export: for each "NAME=value" argument, adds/updates that
** environment variable. Bare "NAME" (no '=') is accepted without
** overwriting an existing value.
** NOTE: this modifies the real process environment directly for now —
** Step 12 replaces this with our OWN managed env list (needed so cd's
** PWD/OLDPWD updates and proper unexported-local-vars work correctly).
*/
static int	builtin_export(char **argv, int argc)
{
	int		i;
	char	*eq;
	char	*name;

	i = 1;
	while (i < argc)
	{
		eq = strchr(argv[i], '=');
		if (eq)
		{
			name = strndup(argv[i], eq - argv[i]);
			setenv(name, eq + 1, 1);
			free(name);
		}
		else
			setenv(argv[i], "", 0);
		i++;
	}
	return (0);
}

/*
** builtin_unset: removes each named environment variable.
*/
static int	builtin_unset(char **argv, int argc)
{
	int	i;

	i = 1;
	while (i < argc)
		unsetenv(argv[i++]);
	return (0);
}

/*
** builtin_env: prints every "NAME=value" pair in the environment.
*/
static int	builtin_env(void)
{
	int	i;

	i = 0;
	while (environ[i])
		printf("%s\n", environ[i++]);
	return (0);
}

/*
** builtin_exit: signals the REPL to stop (via *should_exit) and computes
** the exit status: an explicit numeric argument if given, otherwise the
** status of the last command run — matching bash's "exit" behavior.
*/
static int	builtin_exit(char **argv, int argc, int *should_exit, int last_status)
{
	printf("exit\n");
	*should_exit = 1;
	if (argc >= 2)
		return (atoi(argv[1]) & 0xFF);
	return (last_status);
}

/*
** execute_builtin: dispatches to the right builtin_* based on argv[0].
** Centralizing this here keeps the REPL loop itself simple.
*/
static int	execute_builtin(t_cmd *cmd, int *should_exit, int last_status)
{
	char	*name;

	name = cmd->argv[0];
	if (!strcmp(name, "cd"))
		return (builtin_cd(cmd->argv, cmd->argc));
	if (!strcmp(name, "pwd"))
		return (builtin_pwd());
	if (!strcmp(name, "echo"))
		return (builtin_echo(cmd->argv, cmd->argc));
	if (!strcmp(name, "export"))
		return (builtin_export(cmd->argv, cmd->argc));
	if (!strcmp(name, "unset"))
		return (builtin_unset(cmd->argv, cmd->argc));
	if (!strcmp(name, "env"))
		return (builtin_env());
	return (builtin_exit(cmd->argv, cmd->argc, should_exit, last_status));
}

/* ============================ REPL ============================ */

/*
** repl_loop: reads a line, lexes it, parses it, and (for now) prints
** the resulting pipeline structure so we can verify each stage.
*/
int	repl_loop(void)
{
	char	*line;
	t_token	*tokens;
	t_cmd	*cmds;
	int		err;
	int		exit_status;
	int		should_exit;

	exit_status = 0;
	should_exit = 0;
	set_signals_interactive();
	while (1)
	{
		line = readline("minishell$ ");
		if (!line)
		{
			printf("exit\n");
			break ;
		}
		if (*line)
			add_history(line);
		tokens = tokenize(line, &err);
		if (err)
			printf("minishell: syntax error: unterminated quote\n");
		else if (tokens)
		{
			cmds = parse_tokens(tokens, &err, exit_status);
			if (err)
				printf("minishell: syntax error near unexpected token\n");
			else
			{
				collect_heredocs(cmds, exit_status);
				if (!cmds->next && !cmds->argv)
					exit_status = apply_redirs_only(cmds);
				else if (!cmds->next && !cmds->redirs
					&& is_builtin(cmds->argv[0]))
					exit_status = execute_builtin(cmds, &should_exit,
							exit_status);
				else if (!cmds->next && is_builtin(cmds->argv[0]))
					exit_status = run_builtin_with_redirs(cmds, &should_exit,
							exit_status);
				else
					exit_status = execute_pipeline(cmds);
			}
			free_cmds(cmds);
			free_tokens(tokens);
		}
		free(line);
		if (should_exit)
			break ;
	}
	return (exit_status);
}

int	main(void)
{
	return (repl_loop());
}
