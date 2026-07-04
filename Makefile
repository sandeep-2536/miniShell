NAME		= minishell

CC			= gcc
CFLAGS		= -Wall -Wextra -Werror

SRC_DIR		= src
OBJ_DIR		= obj

SRC			= $(SRC_DIR)/main.c
OBJ			= $(SRC:$(SRC_DIR)/%.c=$(OBJ_DIR)/%.o)

LIBS		= -lreadline

all: $(NAME)

$(NAME): $(OBJ)
	$(CC) $(CFLAGS) $(OBJ) $(LIBS) -o $(NAME)

$(OBJ_DIR)/%.o: $(SRC_DIR)/%.c
	@mkdir -p $(OBJ_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -rf $(OBJ_DIR)

fclean: clean
	rm -f $(NAME)

re: fclean all

.PHONY: all clean fclean re
