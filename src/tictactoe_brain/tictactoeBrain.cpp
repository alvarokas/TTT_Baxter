#include "tictactoeBrain.h"

#include <stdlib.h> // srand, rand

using namespace ttt;
using namespace std;
using namespace baxter_tictactoe;

tictactoeBrain::tictactoeBrain(std::string _name, std::string _strategy, bool legacy_code) : _nh(_name),
                               spinner(4), r(100), _legacy_code(legacy_code), _is_board_detected(false),
                               leftArmCtrl(_name, "left", legacy_code), rightArmCtrl(_name, "right", legacy_code)
{
    ROS_INFO("Legacy code %s enabled.", legacy_code?"is":"is not");
    setBrainState(TTTBrainState::INIT);

    srand(ros::Time::now().nsec);
    setStrategy(_strategy);

    pthread_mutexattr_t _mutex_attr;
    pthread_mutexattr_init(&_mutex_attr);
    pthread_mutexattr_settype(&_mutex_attr, PTHREAD_MUTEX_RECURSIVE_NP);
    pthread_mutex_init(&_mutex_brain, &_mutex_attr);
    pthread_mutex_init(&_mutex_board, &_mutex_attr);

    printf("\n");
    boardState_sub = _nh.subscribe("/baxter_tictactoe/board_state", SUBSCRIBER_BUFFER,
                                    &tictactoeBrain::boardStateCb, this);
    tttBrain_pub   = _nh.advertise<TTTBrainState>("/baxter_tictactoe/ttt_brain_state", 1);

    brainstate_timer = _nh.createTimer(ros::Duration(0.1), &tictactoeBrain::publishTTTBrainState, this, false);

    _nh.param<string>("ttt_brain/voice", _voice_type, VOICE);
    ROS_INFO("Using voice %s", _voice_type.c_str());

    _nh.param<int>("num_games", num_games, NUM_GAMES);

    if (_nh.hasParam("cheating_games"))
    {
        _nh.getParam("cheating_games", cheating_games);
    }
    else
    {
        cheating_games.push_back(CHEATING_GAME_A);
        cheating_games.push_back(CHEATING_GAME_B);
    }

    std::stringstream cheating_games_str;
    std::copy(cheating_games.begin(), cheating_games.end(),
              std::ostream_iterator<int>(cheating_games_str, " "));

    ROS_INFO("Number of games: %i; Cheating games: %s",
              num_games, cheating_games_str.str().c_str());

    string robot_col;
    _nh.param<string>("ttt_brain/robot_color", robot_col, "blue");
    // ROS_ASSERT_MSG(robot_col=="blue", "robot_color should be set to blue in the parameter server. "
    //                           "If you want to use red, be willing to spend some time in coding it!");
    _robot_color=COL_BLUE;
    _opponent_color=_robot_color==COL_BLUE?COL_RED:COL_BLUE;

    ROS_INFO("Robot plays with %s tokens and the opponent with %s tokens.",
              getRobotColor().c_str(), getOpponentColor().c_str());

    // Let's initialize the array of wins [robot, human, tie]
    wins.push_back(0);
    wins.push_back(0);
    wins.push_back(0);

    startInternalThread();
}

void tictactoeBrain::InternalThreadEntry()
{
    bool wipe_out_board_message = true;
    while (ros::ok())
    {
        if      (getBrainState() == TTTBrainState::INIT)
        {
            leftArmCtrl.startAction(ACTION_SCAN);
            setBrainState(TTTBrainState::CALIB);
        }
        else if (getBrainState() == TTTBrainState::CALIB)
        {
            if (leftArmCtrl.getState() == DONE) { setBrainState(TTTBrainState::READY); }
        }
        else if (getBrainState() == TTTBrainState::READY)
        {
            if (_is_board_detected) { setBrainState(TTTBrainState::MATCH_STARTED); }
        }
        else if (getBrainState() == TTTBrainState::MATCH_STARTED)
        {
            saySentence("Welcome!  Let's play Tic Tac Toe.", 4);
            saySentence("Do not grasp your token before I say that it is your turn", 5);
            curr_game = 1;
            setBrainState(TTTBrainState::GAME_STARTED);
        }
        else if (getBrainState() == TTTBrainState::GAME_STARTED)
        {
            if (isBoardEmpty())
            {
                setBrainState(TTTBrainState::GAME_RUNNING);
                wipe_out_board_message = true;
            }
            else if (wipe_out_board_message == true)
            {
                wipe_out_board_message = false;
                saySentence("Please clean the board so that we can start a new game.", 6);
            }
        }
        else if (getBrainState() == TTTBrainState::GAME_RUNNING)
        {
            ROS_WARN("GAME #%i", curr_game);

            playOneGame();

            if (curr_game > num_games) { setBrainState(baxter_tictactoe::TTTBrainState::MATCH_FINISHED); }
            else                       { setBrainState(baxter_tictactoe::TTTBrainState::GAME_STARTED);   }
        }
        else if (getBrainState() == TTTBrainState::MATCH_FINISHED)
        {
            saySentence("Game over. It was my pleasure to win over you. Thanks for being so human.", 10);
            ROS_INFO("Baxter Wins: %i\tHuman Wins: %i\tTies: %i", wins[0], wins[1], wins[2]);
            break;
        }

        r.sleep();
    }
}

void tictactoeBrain::playOneGame()
{
    bool robot_turn = true;
    int winner  = WIN_NONE;

    bool has_to_cheat=false;

    for (size_t j = 0; j < cheating_games.size(); ++j)
    {
        if (cheating_games[j] == curr_game)
        {
            has_to_cheat=true;
        }
    }

    if (has_to_cheat)   { setStrategy("cheating"); }
    else                { setStrategy(   "smart"); }

    saySentence("I start the game.",2);

    // ROS_WARN("PRESS ENTER TO START THE GAME");
    // std::cin.get();
    size_t n_robot_tokens=0;

    while (winner == WIN_NONE && not isBoardFull() && not ros::isShuttingDown())
    {
        if (robot_turn) // Robot's turn
        {
            saySentence("It is my turn", 0.3);
            int cell_toMove = getNextMove();
            ROS_INFO("Moving to cell %i", cell_toMove);

            leftArmCtrl.startAction(ACTION_PICKUP);
            leftArmCtrl.startAction(ACTION_PUTDOWN, cell_toMove);
            ++n_robot_tokens;
        }
        else // Participant's turn
        {
            waitForOpponentTurn(n_robot_tokens);
        }

        robot_turn = not robot_turn;
        winner = getWinner();
    }

    setBrainState(TTTBrainState::GAME_FINISHED);

    switch(winner)
    {
        case WIN_ROBOT:
            ROS_INFO("ROBOT's VICTORY");
            if (has_cheated)
            {
                saySentence("You humans are so easy to beat!", 3);
            }
            saySentence("I won", 2);
            break;
        case WIN_OPP:
            ROS_INFO("OPPONENT's VICTORY");
            saySentence("You won this time", 3);
            break;
        default:
            ROS_INFO("TIE");
            saySentence("That's a tie. I will win next time.", 4);
            winner = WIN_TIE;
    }

    // Let's increment the winners' count
    wins[winner-1] = wins[winner-1] + 1;

    if (has_to_cheat && not has_cheated)
    {
        ROS_WARN("Cheating game ended without cheating. Game counter does not increase.");
        curr_game--;
    }

    curr_game++;

    return;
}

ttt::Board tictactoeBrain::getBoard()
{
    ttt::Board res;

    pthread_mutex_lock(&_mutex_board);
    res = board;
    pthread_mutex_unlock(&_mutex_board);

    return res;
}

void tictactoeBrain::publishTTTBrainState(const ros::TimerEvent&)
{
    pthread_mutex_lock(&_mutex_brain);
    tttBrain_pub.publish(s);
    pthread_mutex_unlock(&_mutex_brain);
}

int tictactoeBrain::getBrainState()
{
    int state;
    pthread_mutex_lock(&_mutex_brain);
    state = s.state;
    pthread_mutex_unlock(&_mutex_brain);

    return state;
}

void tictactoeBrain::setBrainState(int state)
{
    if (state != getBrainState())
    {
        pthread_mutex_lock(&_mutex_brain);
        s.state = state;
        pthread_mutex_unlock(&_mutex_brain);
        ROS_WARN("New state [%i]", state);
    }

    if (state == TTTBrainState::GAME_FINISHED)
    {
        _is_board_detected = false;
    }
}

void tictactoeBrain::boardStateCb(const baxter_tictactoe::MsgBoard &msg)
{
    ROS_DEBUG("New TTT board state received");
    pthread_mutex_lock(&_mutex_board);
    board.fromMsgBoard(msg);
    pthread_mutex_unlock(&_mutex_board);
    _is_board_detected = true;
}

int tictactoeBrain::randomStrategyMove()
{
    int r;
    ttt::Board aux = getBoard();
    do {
        r = rand() % NUMBER_OF_CELLS + 1; //random number between 1 and NUMBER_OF_CELLS
        ROS_DEBUG("Cell %d is in state %s ==? %s", r, aux.getCellState(r-1).c_str(),
                  MsgCell::EMPTY.c_str());
    }
    while(aux.getCellState(r-1)!=COL_EMPTY);

    ROS_WARN("Random move to cell # %i", r);
    return r;
}

int tictactoeBrain::cheatingStrategyMove()
{
    int next_cell_id=-1;
    if (  victoryMove(next_cell_id))    { return next_cell_id; }
    if ( cheatingMove(next_cell_id))    { return next_cell_id; }
    if (defensiveMove(next_cell_id))    { return next_cell_id; }
    return randomStrategyMove();
}

int tictactoeBrain::smartStrategyMove()
{
    int next_cell_id=-1;
    if (  victoryMove(next_cell_id))    { return next_cell_id; }
    if (defensiveMove(next_cell_id))    { return next_cell_id; }
    return randomStrategyMove();
}

bool tictactoeBrain::cheatingMove(int &cell_id)
{
    cell_id = -1;
    ttt::Board aux = getBoard();
    string cell_state = COL_EMPTY;
    for (size_t i = 0; i < aux.getNumCells(); ++i)
    {
        if (aux.getCellState(i)==getOpponentColor())
        {
            cell_state=aux.getCellState(i);
            aux.setCellState(i, getRobotColor());
            if (threeInARow(getRobotColor(), aux))
            {
                ROS_WARN("Cheating move to cell # %lu", i+1);
                has_cheated=true;
                cell_id = i+1;
                return true;
            }
            aux.setCellState(i, cell_state);
        }
    }

    // ROS_WARN("Cheating move not successful!");
    return false;
}

bool tictactoeBrain::defensiveMove(int &cell_id)
{
    cell_id = -1;
    ttt::Board aux = getBoard();
    string cell_state = COL_EMPTY;
    for (size_t i = 0; i < aux.getNumCells(); ++i)
    {
        if (aux.getCellState(i)==COL_EMPTY)
        {
            cell_state = aux.getCellState(i);
            aux.setCellState(i, getOpponentColor());
            if (threeInARow(getOpponentColor(), aux))
            {
                ROS_WARN("Defensive move to cell # %lu", i+1);
                cell_id = i+1;
                return true;
            }
            aux.setCellState(i, cell_state);
        }
    }

    // ROS_INFO("DefensiveMove not successful!");
    return false;
}

bool tictactoeBrain::victoryMove(int &cell_id)
{
    cell_id = -1;
    ttt::Board aux = getBoard();
    string cell_state = COL_EMPTY;

    for (size_t i = 0; i < aux.getNumCells(); ++i)
    {
        if (aux.getCellState(i)==COL_EMPTY)
        {
            cell_state = aux.getCellState(i);
            aux.setCellState(i, getRobotColor());
            if (threeInARow(getRobotColor(), aux))
            {
                ROS_WARN("Victory move to cell # %lu", i+1);
                cell_id = i+1;
                return true;
            }
            aux.setCellState(i, cell_state);
        }
    }

    // ROS_INFO("VictoryMove not successful!");
    return false;
}

int tictactoeBrain::getNextMove()
{
    return (this->*_choose_next_move)();
}

unsigned short int tictactoeBrain::getNumTokens()
{
    unsigned short int counter=0;
    ttt::Board aux = getBoard();

    for (size_t i = 0; i < aux.getNumCells(); i++)
    {
        if(aux.getCellState(i)!=COL_EMPTY) { counter++; }
    }
    return counter;
}

unsigned short int tictactoeBrain::getNumTokens(const std::string& token_type)
{
    unsigned short int counter=0;
    ttt::Board aux = getBoard();

    for (size_t i = 0; i < aux.getNumCells(); i++)
    {
       if(aux.getCellState(i)==token_type) { counter++; }
    }
    return counter;
}

bool tictactoeBrain::threeInARow(const std::string& col, ttt::Board& b)
{
    if(col!=COL_BLUE && col!=COL_RED) { return false; } // There are only red and blue tokens

    if(b.getCellState(0)==col && b.getCellState(1)==col && b.getCellState(2)==col) { return true; } // first row
    if(b.getCellState(3)==col && b.getCellState(4)==col && b.getCellState(5)==col) { return true; } // second row
    if(b.getCellState(6)==col && b.getCellState(7)==col && b.getCellState(8)==col) { return true; } // third row
    if(b.getCellState(0)==col && b.getCellState(3)==col && b.getCellState(6)==col) { return true; } // first column
    if(b.getCellState(1)==col && b.getCellState(4)==col && b.getCellState(7)==col) { return true; } // second column
    if(b.getCellState(2)==col && b.getCellState(5)==col && b.getCellState(8)==col) { return true; } // third column
    if(b.getCellState(0)==col && b.getCellState(4)==col && b.getCellState(8)==col) { return true; } // first diagonal
    if(b.getCellState(2)==col && b.getCellState(4)==col && b.getCellState(6)==col) { return true; } // second diagonal

    return false;
}

unsigned short int tictactoeBrain::getWinner()
{
    ttt::Board aux = getBoard();

    if (threeInARow(getRobotColor(), aux))      { return WIN_ROBOT; }
    if (threeInARow(getOpponentColor(), aux))   { return   WIN_OPP; }

    return WIN_NONE;
}

void tictactoeBrain::waitForOpponentTurn(const size_t& n_robot_tokens)
{
    ROS_INFO("Waiting for the participant's move. "
             "I am expecting %lu token%s from myself and %lu token%s from my opponent",
             n_robot_tokens, n_robot_tokens==1?"":"s",
             n_robot_tokens, n_robot_tokens==1?"":"s");

    int cnt = 0;
    bool say_it_is_your_turn = true;

    // We wait until the number of opponent's tokens equals the robots'
    while(ros::ok())
    {
        if (getNumTokens(getOpponentColor()) == n_robot_tokens && getNumTokens(getRobotColor()) == n_robot_tokens)
        {
            ++cnt;
            // ROS_INFO_THROTTLE(1, "Correct number of tiles! Cnt %i", cnt);
        }
        else
        {
            if (say_it_is_your_turn == true)
            {
                saySentence("It is your turn", 0.1);
                say_it_is_your_turn = false;
            }

            cnt = 0;
        }

        if (cnt == 100) { return; } // 100 means 1 second

        r.sleep();
    }
}

bool tictactoeBrain::isBoardEmpty()
{
    return getBoard().isEmpty();
}

bool tictactoeBrain::isBoardFull()
{
    return getBoard().isFull();
}

void tictactoeBrain::saySentence(std::string sentence, double t)
{
    ROS_INFO("saySentence: %s", sentence.c_str());
    _voice_synthesizer.say(sentence, _voice_type);
    ros::Duration(t).sleep();
}

void tictactoeBrain::setStrategy(std::string strategy)
{
    if (strategy=="random")
    {
        _choose_next_move=&tictactoeBrain::randomStrategyMove;
        ROS_INFO("[strategy] Randomly place tokens");
    }
    else if (strategy=="smart")
    {
        _choose_next_move=&tictactoeBrain::smartStrategyMove;
        ROS_INFO("[strategy] Try to win without cheating");
    }
    else if (strategy=="cheating")
    {
        _choose_next_move=&tictactoeBrain::cheatingStrategyMove;
        ROS_INFO("[strategy] Try to win by cheating");
    }
    else
    {
        ROS_ERROR("%s is not an available strategy", strategy.c_str());
    }
}

tictactoeBrain::~tictactoeBrain()
{
    pthread_mutex_destroy(&_mutex_brain);
    pthread_mutex_destroy(&_mutex_board);
    brainstate_timer.stop();
}
